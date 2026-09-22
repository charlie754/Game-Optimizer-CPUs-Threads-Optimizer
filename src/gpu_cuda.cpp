// Game Optimizer - which GPU CUDA uses, the NVIDIA implementation. See gpu_cuda.h for the rules.
//
// NOTHING HERE IS LINKED AT LOAD TIME. nvapi64.dll is opened with LoadLibraryEx and every function is
// looked up by its interface id through nvapi_QueryInterface, exactly as gpu_pref.cpp resolves DXCore,
// so this executable gains no import and still starts on a machine with no NVIDIA driver at all.
//
// 🔴 NvAPI_Unload IS NEVER CALLED. [M] 2026-09-19: after it, ANY further NvAPI call crashes the process -
// NvAPI_GetErrorMessage included, so even formatting the status of the call that noticed is fatal. The
// library is initialised once and kept for the life of the process; only the DRS SESSION is opened and
// closed per action.
//
// THE STRUCT LAYOUTS AND INTERFACE IDS ARE NVIDIA'S, from the MIT-licensed headers at
// github.com/NVIDIA/nvapi (see NOTICE.md). They are re-declared here rather than vendored so this file
// stays self-contained; every size is pinned by a static_assert, so a layout typed wrong is a build
// failure and not a corrupt driver database.
#include "gpu_cuda.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>

#include "util.h"

namespace cd {
namespace {

typedef uint32_t NvU32;
typedef uint16_t NvU16;
typedef uint8_t NvU8;
typedef uint64_t NvU64;
typedef int NvAPI_Status;
typedef NvU16 NvAPI_UnicodeString[2048];
typedef void* NvDRSSessionHandle;
typedef void* NvDRSProfileHandle;
typedef void* NvPhysicalGpuHandle;

const NvAPI_Status NVAPI_OK = 0;
const NvAPI_Status NVAPI_EXECUTABLE_ALREADY_IN_USE = -167;
// The driver's own way of saying "no settings entry of mine covers that executable" - the ONE answer a
// lookup may read as Absent. Everything else it can answer is Failed (gpu_cuda.h, CudaLookup).
const NvAPI_Status NVAPI_EXECUTABLE_NOT_FOUND = -166;
const NvAPI_Status NVAPI_SETTING_NOT_FOUND = -160;
// The driver's own way of saying "I have no profile of that name" - the ONE answer findProfileByName may
// read as Absent (R5-2). Every other unhappy answer is Failed, and nothing is adopted or created on Failed.
const NvAPI_Status NVAPI_PROFILE_NOT_FOUND = -163;
// The driver's own way of saying "that is every one of them" to an enumeration that ran off the end. It
// is a SUCCESSFUL end, not a refusal, and reading it as one would report every profile's settings as
// unlistable and refuse every Remove (R6-1).
const NvAPI_Status NVAPI_END_ENUMERATION = -8;
const int NVAPI_MAX_PHYSICAL_GPUS = 64;
// CUDA_EXCLUDED_GPUS_ID, "List of Universal GPU ids". 🔴 R6-1: TAKEN FROM THE HEADER, NOT TYPED AGAIN.
// The delete decision now compares enumerated setting ids against this number in gpu_cuda.h, so two
// copies of it could drift and the drift would read as "this entry holds a setting we did not write".
const NvU32 kCudaExcludedGpus = static_cast<NvU32>(CudaSettingId());
const NvU32 kOglImplicitAffinity = 0x20D0F3E6; // OGL_IMPLICIT_GPU_AFFINITY_ID: the id strings live here
const int kWideStringType = 3;                 // NVDRS_WSTRING_TYPE

#define MAKE_NVAPI_VERSION(t, v) (NvU32)(sizeof(t) | ((v) << 16))

typedef struct { NvU32 valueLength; NvU8 valueData[4096]; } NVDRS_BINARY_SETTING;
#pragma pack(push, 4)
typedef struct {
    NvU32 version; NvU32 numSettingValues; int settingType;
    union { NvU32 u32DefaultValue; NVDRS_BINARY_SETTING binaryDefaultValue;
            NvAPI_UnicodeString wszDefaultValue; NvU64 u64DefaultValue; };
    union { NvU32 u32Value; NVDRS_BINARY_SETTING binaryValue;
            NvAPI_UnicodeString wszValue; NvU64 u64Value; } settingValues[100];
} NVDRS_SETTING_VALUES;
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
static_assert(sizeof(NVDRS_SETTING_VALUES) == 414112, "NVDRS_SETTING_VALUES size");
static_assert(sizeof(NVDRS_PROFILE_V1) == 4116, "NVDRS_PROFILE_V1 size");

// Every interface id below resolved on this machine [M] 2026-09-19.
enum : NvU32 {
    ID_Initialize = 0x0150e828,
    ID_EnumPhysicalGPUs = 0xe5ac921f,
    ID_GPU_GetPCIIdentifiers = 0x2ddfb66e,
    ID_GPU_GetBusId = 0x1be0b8e5,
    ID_DRS_CreateSession = 0x0694d52e,
    ID_DRS_DestroySession = 0xdad9cff8,
    ID_DRS_LoadSettings = 0x375dbd6b,
    ID_DRS_SaveSettings = 0xfcbc7e14,
    ID_DRS_CreateProfile = 0xcc176068,
    ID_DRS_GetProfileInfo = 0x61cd6fd6,
    ID_DRS_FindProfileByName = 0x7e4a9a0b,
    ID_DRS_CreateApplication = 0x4347a9de,
    ID_DRS_EnumApplications = 0x7fa2173a,
    ID_DRS_FindApplicationByName = 0xeee566b2,
    ID_DRS_SetSetting = 0x577dd202,
    ID_DRS_GetSetting = 0x73bf8338,
    ID_DRS_DeleteProfileSetting = 0xe4a26362,
    ID_DRS_EnumAvailableSettingValues = 0x2ec39f90,
    // 🔴 R6-1: WHICH SETTINGS A PROFILE HOLDS OF ITS OWN. [A] This interface id is taken from the same
    // MIT-licensed NVIDIA headers as every other one here and has NOT been resolved on this machine - the
    // probe that would measure it is the chair's to run, not this lane's. The failure is closed either
    // way: an id that does not resolve leaves NvApi::enumSettings null, ApplyCudaDriverCalls then leaves
    // CudaOps::listSettingIds EMPTY, and every Remove that would have deleted an entry refuses with
    // SettingsUnknown and says so instead. Nothing is deleted on a guess.
    ID_DRS_EnumSettings = 0xae3039da,
    // Only ever used on a profile this feature made itself, and only through the three guards in
    // DeleteOwnProfile below - the same ones tests\cuda_assign_probe.cpp cleans up with.
    ID_DRS_DeleteProfile = 0x17093206,
    ID_DRS_DeleteApplicationEx = 0xc5ea85a1,
};

typedef void* (__cdecl* QueryInterface_t)(NvU32);
typedef NvAPI_Status(__cdecl* F_Void)();
typedef NvAPI_Status(__cdecl* F_EnumGpus)(NvPhysicalGpuHandle*, NvU32*);
typedef NvAPI_Status(__cdecl* F_GpuPci)(NvPhysicalGpuHandle, NvU32*, NvU32*, NvU32*, NvU32*);
typedef NvAPI_Status(__cdecl* F_GpuU32)(NvPhysicalGpuHandle, NvU32*);
typedef NvAPI_Status(__cdecl* F_CreateSess)(NvDRSSessionHandle*);
typedef NvAPI_Status(__cdecl* F_Sess)(NvDRSSessionHandle);
typedef NvAPI_Status(__cdecl* F_ProfInfo)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_PROFILE_V1*);
typedef NvAPI_Status(__cdecl* F_FindProf)(NvDRSSessionHandle, NvU16*, NvDRSProfileHandle*);
typedef NvAPI_Status(__cdecl* F_CreateProf)(NvDRSSessionHandle, NVDRS_PROFILE_V1*, NvDRSProfileHandle*);
typedef NvAPI_Status(__cdecl* F_App)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_APPLICATION_V4*);
typedef NvAPI_Status(__cdecl* F_EnumApps)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NvU32*,
                                          NVDRS_APPLICATION_V4*);
typedef NvAPI_Status(__cdecl* F_FindApp)(NvDRSSessionHandle, NvU16*, NvDRSProfileHandle*,
                                         NVDRS_APPLICATION_V4*);
typedef NvAPI_Status(__cdecl* F_Setting)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING_V1*);
typedef NvAPI_Status(__cdecl* F_GetSetting)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NVDRS_SETTING_V1*);
typedef NvAPI_Status(__cdecl* F_DelSetting)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32);
typedef NvAPI_Status(__cdecl* F_EnumVals)(NvU32, NvU32*, NVDRS_SETTING_VALUES*);
typedef NvAPI_Status(__cdecl* F_EnumSettings)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NvU32*,
                                              NVDRS_SETTING_V1*);
typedef NvAPI_Status(__cdecl* F_DelProfile)(NvDRSSessionHandle, NvDRSProfileHandle);
typedef NvAPI_Status(__cdecl* F_DelApp)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_APPLICATION_V4*);

// Every entry point this file uses, resolved once. `ready` is false when the driver is absent or any
// one of them is missing: a partly resolved NVAPI is refused outright rather than used half-way.
struct NvApi {
    bool ready = false;
    F_EnumGpus enumGpus = nullptr;
    F_GpuPci gpuPci = nullptr;
    F_GpuU32 gpuBus = nullptr;
    F_CreateSess createSession = nullptr;
    F_Sess destroySession = nullptr;
    F_Sess loadSettings = nullptr;
    F_Sess saveSettings = nullptr;
    F_CreateProf createProfile = nullptr;
    F_ProfInfo profileInfo = nullptr;
    F_FindProf findProfile = nullptr;
    F_App createApp = nullptr;
    F_EnumApps enumApps = nullptr;
    F_FindApp findApp = nullptr;
    F_Setting setSetting = nullptr;
    F_GetSetting getSetting = nullptr;
    F_DelSetting deleteSetting = nullptr;
    F_EnumVals enumValues = nullptr;
    // 🔴 NOT PART OF `ready`, ON PURPOSE. These exist to take an entry away again and to say which settings
    // one holds; a driver that does not offer them must still be able to SET the CUDA GPU, which is the
    // whole feature.
    // 🔴 R6-2: AND A MISSING ONE NOW REALLY REACHES THE RULES THAT ASK ABOUT IT. MakeCudaOps used to
    // install a delete lambda whatever these held, so PlanCudaForRow's "this driver offers no way to take
    // a settings entry away again" could never fire on a real machine - a rule written where the tests
    // reach, against a seam the .cpp always filled. ApplyCudaDriverCalls (gpu_cuda.h) is now the one place
    // that decides, and it leaves the operation EMPTY instead.
    F_DelProfile deleteProfile = nullptr;
    F_DelApp deleteApp = nullptr;
    F_EnumSettings enumSettings = nullptr;
};

// 🔴 R6-2: WHAT THIS DRIVER CAN DO, AS ONE STRUCT THE HEADER RULES ON. Nothing is decided here.
CudaDriverCalls CallsOf(const NvApi& a) {
    CudaDriverCalls c;
    c.deleteProfile = a.deleteProfile != nullptr;
    c.deleteApplication = a.deleteApp != nullptr;
    c.enumSettings = a.enumSettings != nullptr;
    return c;
}

// 🔴 SYSTEM32 ONLY. A name-only LoadLibrary searches the application's own folder first for a DLL that
// is not a KnownDLL, so an nvapi64.dll dropped beside the exe would be the one loaded - and this one is
// handed the path of every application the user ticks. [M] the driver installs it in System32.
// INITIALISED ONCE AND NEVER UNLOADED, for the reason at the top of this file.
const NvApi& Api() {
    static NvApi api = []() {
        NvApi a;
        const HMODULE m = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m == nullptr) {
            LogLine(L"[cuda] nvapi64.dll is not present in System32 (gle=%lu); CUDA assignment is off",
                    GetLastError());
            return a;
        }
        const QueryInterface_t qi =
            reinterpret_cast<QueryInterface_t>(reinterpret_cast<void*>(GetProcAddress(m, "nvapi_QueryInterface")));
        if (qi == nullptr) {
            LogLine(L"[cuda] nvapi64.dll has no nvapi_QueryInterface; CUDA assignment is off");
            return a;
        }
        const F_Void init = reinterpret_cast<F_Void>(qi(ID_Initialize));
        a.enumGpus = reinterpret_cast<F_EnumGpus>(qi(ID_EnumPhysicalGPUs));
        a.gpuPci = reinterpret_cast<F_GpuPci>(qi(ID_GPU_GetPCIIdentifiers));
        a.gpuBus = reinterpret_cast<F_GpuU32>(qi(ID_GPU_GetBusId));
        a.createSession = reinterpret_cast<F_CreateSess>(qi(ID_DRS_CreateSession));
        a.destroySession = reinterpret_cast<F_Sess>(qi(ID_DRS_DestroySession));
        a.loadSettings = reinterpret_cast<F_Sess>(qi(ID_DRS_LoadSettings));
        a.saveSettings = reinterpret_cast<F_Sess>(qi(ID_DRS_SaveSettings));
        a.createProfile = reinterpret_cast<F_CreateProf>(qi(ID_DRS_CreateProfile));
        a.profileInfo = reinterpret_cast<F_ProfInfo>(qi(ID_DRS_GetProfileInfo));
        a.findProfile = reinterpret_cast<F_FindProf>(qi(ID_DRS_FindProfileByName));
        a.createApp = reinterpret_cast<F_App>(qi(ID_DRS_CreateApplication));
        a.enumApps = reinterpret_cast<F_EnumApps>(qi(ID_DRS_EnumApplications));
        a.findApp = reinterpret_cast<F_FindApp>(qi(ID_DRS_FindApplicationByName));
        a.setSetting = reinterpret_cast<F_Setting>(qi(ID_DRS_SetSetting));
        a.getSetting = reinterpret_cast<F_GetSetting>(qi(ID_DRS_GetSetting));
        a.deleteSetting = reinterpret_cast<F_DelSetting>(qi(ID_DRS_DeleteProfileSetting));
        a.enumValues = reinterpret_cast<F_EnumVals>(qi(ID_DRS_EnumAvailableSettingValues));
        a.deleteProfile = reinterpret_cast<F_DelProfile>(qi(ID_DRS_DeleteProfile));
        a.deleteApp = reinterpret_cast<F_DelApp>(qi(ID_DRS_DeleteApplicationEx));
        a.enumSettings = reinterpret_cast<F_EnumSettings>(qi(ID_DRS_EnumSettings));
        const bool all = init && a.enumGpus && a.gpuPci && a.gpuBus && a.createSession && a.destroySession &&
                         a.loadSettings && a.saveSettings && a.createProfile && a.profileInfo && a.findProfile &&
                         a.createApp && a.enumApps && a.findApp && a.setSetting && a.getSetting &&
                         a.deleteSetting && a.enumValues;
        if (!all) {
            LogLine(L"[cuda] this NVIDIA driver does not offer every settings function this needs; "
                    L"CUDA assignment is off");
            return a;
        }
        const NvAPI_Status s = init();
        if (s != NVAPI_OK) {
            LogLine(L"[cuda] NvAPI_Initialize refused (%d); CUDA assignment is off", s);
            return a;
        }
        // 🔴 R6-2: SAID OUT LOUD, because a capability that is missing now changes what the product will
        // do - a driver with no delete calls refuses to make an entry at all, and one with no EnumSettings
        // refuses to take one away. A silent "it did nothing" would be indistinguishable from a bug.
        LogLine(L"[cuda] optional driver calls: DeleteProfile=%d DeleteApplicationEx=%d EnumSettings=%d",
                a.deleteProfile != nullptr ? 1 : 0, a.deleteApp != nullptr ? 1 : 0,
                a.enumSettings != nullptr ? 1 : 0);
        a.ready = true;
        return a;
    }();
    return api;
}

std::wstring FromUnicodeString(const NvU16* w) {
    if (w == nullptr) return std::wstring();
    const wchar_t* p = reinterpret_cast<const wchar_t*>(w);
    size_t n = 0;
    while (n < 2048 && p[n] != L'\0') ++n;
    return std::wstring(p, n);
}

void ToUnicodeString(NvU16* dst, const std::wstring& src) {
    std::memset(dst, 0, 2048 * sizeof(NvU16));
    const size_t n = src.size() < 2046 ? src.size() : 2046;
    if (n) std::memcpy(dst, src.c_str(), n * sizeof(wchar_t));
}

// One DRS session, open for as long as one Apply or Remove runs. Held by shared_ptr so every op below
// keeps it alive even if the caller drops the CudaOps first.
struct Session {
    NvDRSSessionHandle h = nullptr;
    bool open = false;
};

NvDRSProfileHandle ProfileByName(const std::shared_ptr<Session>& s, const std::wstring& name) {
    if (!s->open || name.empty()) return nullptr;
    NvAPI_UnicodeString buf;
    ToUnicodeString(buf, name);
    NvDRSProfileHandle h = nullptr;
    if (Api().findProfile(s->h, buf, &h) != NVAPI_OK) return nullptr;
    return h;
}

// How many OTHER application entries `profile` covers, `except` left out. Read in pages of eight, which is
// what the probe used; a profile with more is read in several passes.
//
// 🔴 A COUNT, NOT A LIST, SINCE v0.5.8's E1. The list existed to name a shared profile's neighbours in the
// question before the user agreed to move their CUDA setting too; this product never moves them any more,
// and the only thing left to say about an entry it is REFUSING is how many programs go with it.
//
// 🔴 R6-6: THIS FUNCTION REPORTS WHAT THE DRIVER SAID. IT DOES NOT JUDGE IT. Until round 6 it did both,
// and it measured the two halves DIFFERENTLY: it totalled the RAW rows to decide "did we see all of them"
// and de-duplicated the NAMES to decide how many other programs there are. A driver that handed one entry
// back twice therefore satisfied "we saw them all" while a real member had never been listed at all - and
// that entry was then written on, or deleted, as though its membership were known. An empty name was
// silently skipped, which did the same thing with fewer steps.
//
// Every name the driver hands back now goes into `listed`, in order, blanks and repeats included, and
// JudgeCudaMembership (gpu_cuda.h, where the unit suite reaches it) decides what that adds up to.
// `expected` is NVDRS_PROFILE::numOfApps, which the driver states before any page is read.
// ponytail: it still stops after 256 entries, so a profile with more is reported INCOMPLETE and its rows
// are refused rather than under-reported; a growing buffer would lift the cap. [M] the largest profile on
// this machine covers six.
size_t OtherAppCountOf(const std::shared_ptr<Session>& s, NvDRSProfileHandle profile,
                       const std::wstring& except, NvU32 expected, bool& complete) {
    complete = false;
    std::vector<std::wstring> listed;
    if (!s->open || profile == nullptr) return 0;
    std::vector<NVDRS_APPLICATION_V4> page(8);
    // The walk is WalkCudaPages (gpu_cuda.h). Any refusal still goes to JudgeCudaMembership, as it always
    // did: a walk that stopped short lists fewer entries than `expected`, and that is judged incomplete there.
    const CudaWalk walk = WalkCudaPages(
        static_cast<unsigned long>(page.size()), 256,
        [&](unsigned long start, unsigned long& count) {
            std::memset(&page[0], 0, page.size() * sizeof(NVDRS_APPLICATION_V4));
            for (size_t k = 0; k < page.size(); ++k) page[k].version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
            NvU32 n = static_cast<NvU32>(count);
            const NvAPI_Status got = Api().enumApps(s->h, profile, static_cast<NvU32>(start), &n, &page[0]);
            count = n;
            return got == NVAPI_OK ? CudaPage::Filled : CudaPage::Refused;
        },
        [&](unsigned long k) {
            listed.push_back(FromUnicodeString(page[k].appName));   // blanks included: they are an answer
        });
    if (walk == CudaWalk::Overran) {
        // 🔴 NOT JUDGED AT ALL: entries were skipped, so no count made from what was kept is the real one.
        LogLine(L"[cuda] the driver reported more applications in one page than it was given room for");
        return 0;   // `complete` is still false
    }
    const CudaMembership m = JudgeCudaMembership(listed, expected, except);
    complete = m.complete;
    return m.otherApps;
}

// 🔴 R6-1: WHICH SETTINGS A PROFILE HOLDS OF ITS OWN, AS IDS. Reported, never judged - JudgeCudaSettings
// in gpu_cuda.h decides what they mean. False means the driver would not say, which the caller must never
// read as "it holds none": an empty enumeration and a refused one are opposite facts about the same call,
// exactly as they are for the setting read and the profile lookup.
// ponytail: it stops after 256 settings, and a profile with more answers "could not list"; [M] no profile
// on this machine carries anything near that.
bool SettingIdsOf(const std::shared_ptr<Session>& s, NvDRSProfileHandle profile,
                  std::vector<unsigned long>& ids) {
    ids.clear();
    if (!s->open || profile == nullptr || Api().enumSettings == nullptr) return false;
    // NVDRS_SETTING_V1 is 12,320 bytes, so the page is small and on the heap.
    const size_t kPage = 4;
    std::vector<NVDRS_SETTING_V1> page(kPage);
    NvAPI_Status got = NVAPI_OK;
    // The walk is WalkCudaPages (gpu_cuda.h): NVAPI_END_ENUMERATION is the driver saying "that is all of
    // them", and a page the driver says is longer than its buffer is Overran - never a short list.
    const CudaWalk walk = WalkCudaPages(
        static_cast<unsigned long>(kPage), 256,
        [&](unsigned long start, unsigned long& count) {
            std::memset(&page[0], 0, page.size() * sizeof(NVDRS_SETTING_V1));
            for (size_t k = 0; k < page.size(); ++k) page[k].version = MAKE_NVAPI_VERSION(NVDRS_SETTING_V1, 1);
            NvU32 n = static_cast<NvU32>(count);
            got = Api().enumSettings(s->h, profile, static_cast<NvU32>(start), &n, &page[0]);
            count = n;
            if (got == NVAPI_END_ENUMERATION) return CudaPage::End;
            return got == NVAPI_OK ? CudaPage::Filled : CudaPage::Refused;
        },
        [&](unsigned long k) { ids.push_back(static_cast<unsigned long>(page[k].settingId)); });
    if (walk == CudaWalk::Complete) return true;
    if (walk == CudaWalk::Refused)
        LogLine(L"[cuda] the settings of a profile could not be listed (%d)", got);
    else if (walk == CudaWalk::Overran)
        LogLine(L"[cuda] the driver reported more settings in one page than it was given room for, so the "
                L"profile's settings are reported unknown");
    else   // the cap was reached, so this is not the whole list and must not be answered as if it were
        LogLine(L"[cuda] a profile holds more settings than this reads, so its settings are reported unknown");
    ids.clear();
    return false;
}

// 🔴 THE ONLY PLACE THIS PRODUCT DELETES ANYTHING FROM THE DRIVER DATABASE, AND IT REFUSES FIRST.
// Four guards, all of them asked against what the driver says RIGHT NOW rather than against what the
// caller believes - the same ones tests\cuda_assign_probe.cpp cleans up behind itself with:
//   1. the name is one this feature makes ("Game Optimizer - <path>"), and the profile the driver found
//      still carries exactly that name;
//   2. NVIDIA did not ship the profile itself;
//   3. it covers no more than one application, so nothing else is losing its settings with it;
//   4. 🔴 and that ONE application is `appEntry`, the entry the record names (E1). A count says nothing
//      about WHICH application it counted, so a profile that lost our entry and gained somebody else's
//      passed guard 3 on its own - and deleting it would have taken that program's settings with it.
// The CALLER has its own guard this cannot see: it may only ask when the create call really created the
// profile, because a profile already carrying this name was reused and a reused one is not ours.
// A profile that fails any of them is left alone, and the caller treats that as "not tidied up", never
// as a failure of the change it follows.
bool DeleteOwnProfile(const std::shared_ptr<Session>& s, const std::wstring& profileName,
                      const std::wstring& appEntry) {
    if (!s->open || appEntry.empty() || !IsCudaProfileWeMade(profileName)) return false;
    if (Api().deleteProfile == nullptr || Api().deleteApp == nullptr) return false;
    const NvDRSProfileHandle handle = ProfileByName(s, profileName);
    if (handle == nullptr) return false;
    NVDRS_PROFILE_V1 info;
    std::memset(&info, 0, sizeof info);
    info.version = MAKE_NVAPI_VERSION(NVDRS_PROFILE_V1, 1);
    if (Api().profileInfo(s->h, handle, &info) != NVAPI_OK) return false;
    if (!IEquals(FromUnicodeString(info.profileName), profileName)) return false;   // R5-10
    if (info.isPredefined != 0 || info.numOfApps > 1) return false;
    // 🔴 R5-1 / R6-1, GUARD 5, AND IT IS THE ROUND-4 BLOCKER MEASURED IN THIS FUNCTION - TWICE. First
    // numOfSettings sat in the struct above unread, so an entry of ours that the user had also given some
    // other NVIDIA setting was deleted with that setting in it. Then it was read as `numOfSettings > 1`,
    // which is a COUNT standing in for an IDENTITY: "one setting" says nothing about WHICH setting, so an
    // entry holding the user's "Vertical sync" and no CUDA setting of its own still went whole.
    //
    // The settings are enumerated instead, and the rule is that NOTHING THIS PRODUCT DID NOT WRITE MAY BE
    // IN THERE. Zero settings is allowed and has to be: the clean-up after a half-finished create (E7) and
    // the rollback after a cleared setting both hand this an entry with none. An enumeration that could
    // not be made refuses, as every other unreadable answer in this feature does.
    std::vector<unsigned long> settingIds;
    if (!SettingIdsOf(s, handle, settingIds)) return false;
    for (size_t i = 0; i < settingIds.size(); ++i)
        if (settingIds[i] != CudaSettingId()) return false;
    bool complete = false;
    // Everything it covers EXCEPT our own entry. Zero means the only application on it is ours - or that
    // it has none at all, which is a profile a half-finished create left behind and is ours to take away.
    const size_t others = OtherAppCountOf(s, handle, appEntry, info.numOfApps, complete);
    if (!complete) return false;                       // an entry we cannot see is an entry we cannot judge
    if (others != 0) return false;                     // guard 4: the one application on it is not ours
    // 🔴 THE APPLICATION ENTRY IS ONLY REMOVED WHEN THERE IS ONE, AND ITS ANSWER IS READ (Council round 3:
    // this result used to be ignored). An empty profile - what E7 takes away after a failed
    // CreateApplication - has nothing to remove, and asking anyway would answer no and abandon the delete.
    if (info.numOfApps > 0) {
        std::unique_ptr<NVDRS_APPLICATION_V4> app(new NVDRS_APPLICATION_V4());
        std::memset(app.get(), 0, sizeof(NVDRS_APPLICATION_V4));
        app->version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
        ToUnicodeString(app->appName, appEntry);
        if (Api().deleteApp(s->h, handle, app.get()) != NVAPI_OK) return false;
    }
    return Api().deleteProfile(s->h, handle) == NVAPI_OK;
}

}  // namespace

// ---- The live driver ---------------------------------------------------------------------------
CudaOps MakeCudaOps() {
    CudaOps ops;
    const NvApi& api = Api();
    if (!api.ready) {
        ops.openRefusal = CudaRefusal::NoNvidiaDriver;
        return ops;
    }
    std::shared_ptr<Session> s(new Session());
    if (api.createSession(&s->h) != NVAPI_OK || s->h == nullptr) {
        ops.openRefusal = CudaRefusal::SessionRefused;
        LogLine(L"[cuda] NVIDIA's settings session could not be created");
        return ops;
    }
    const NvAPI_Status loaded = api.loadSettings(s->h);
    if (loaded != NVAPI_OK) {
        api.destroySession(s->h);
        ops.openRefusal = CudaRefusal::SessionRefused;
        LogLine(L"[cuda] NVIDIA's settings could not be loaded (%d)", loaded);
        return ops;
    }
    s->open = true;
    ops.available = true;

    ops.listGpus = [s]() {
        std::vector<NvidiaGpu> out;
        NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS];
        std::memset(handles, 0, sizeof handles);
        NvU32 n = 0;
        if (Api().enumGpus(handles, &n) != NVAPI_OK) return out;
        for (NvU32 i = 0; i < n && i < NVAPI_MAX_PHYSICAL_GPUS; ++i) {
            NvU32 device = 0, subsys = 0, revision = 0, ext = 0, bus = 0;
            if (Api().gpuPci(handles[i], &device, &subsys, &revision, &ext) != NVAPI_OK) continue;
            if (Api().gpuBus(handles[i], &bus) != NVAPI_OK) continue;
            wchar_t key[32];
            // EXACTLY AdapterKeyFromPnpId's form: the low word of deviceId is the PCI vendor, the high
            // word is the device, and subSystemId is the subsystem [M] nvmap.
            swprintf_s(key, 32, L"%04X&%04X&%08X", device & 0xFFFFu, (device >> 16) & 0xFFFFu, subsys);
            NvidiaGpu g;
            g.adapterKey = key;
            g.busId = bus;
            out.push_back(g);
        }
        return out;
    };

    ops.listIds = [s]() {
        std::vector<std::wstring> out;
        std::unique_ptr<NVDRS_SETTING_VALUES> values(new NVDRS_SETTING_VALUES());
        std::memset(values.get(), 0, sizeof(NVDRS_SETTING_VALUES));
        values->version = MAKE_NVAPI_VERSION(NVDRS_SETTING_VALUES, 1);
        NvU32 max = 100;
        // 🔴 0x20D0F3E6, NOT 0x10354FF8. [M] the CUDA setting enumerates only "none"; the OpenGL one
        // enumerates the driver's real per-GPU strings, which is where they must be copied from.
        if (Api().enumValues(kOglImplicitAffinity, &max, values.get()) != NVAPI_OK) return out;
        const NvU32 n = values->numSettingValues < 100 ? values->numSettingValues : 100;
        for (NvU32 k = 0; k < n; ++k) {
            const std::wstring v = FromUnicodeString(values->settingValues[k].wszValue);
            if (!v.empty()) out.push_back(v);
        }
        return out;
    };

    ops.findProfileForExe = [s](const std::wstring& exePath, CudaProfile& p) {
        p = CudaProfile();
        if (!s->open) return CudaLookup::Failed;
        NvAPI_UnicodeString name;
        ToUnicodeString(name, CudaAppKeyFor(exePath));
        std::unique_ptr<NVDRS_APPLICATION_V4> app(new NVDRS_APPLICATION_V4());
        std::memset(app.get(), 0, sizeof(NVDRS_APPLICATION_V4));
        app->version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
        NvDRSProfileHandle handle = nullptr;
        const NvAPI_Status got = Api().findApp(s->h, name, &handle, app.get());
        // THE ONE ANSWER THAT MEANS "NVIDIA HAS NO ENTRY FOR THIS PROGRAM" IS THE DRIVER SAYING SO, exactly
        // as readSetting takes only NVAPI_SETTING_NOT_FOUND for "there is no such setting". Every other
        // unhappy answer - a refused lookup, a handle the call left null, a profile whose own details could
        // not be read - is Failed, and Failed makes nothing (gpu_cuda.h, CudaLookup).
        if (got == NVAPI_EXECUTABLE_NOT_FOUND) return CudaLookup::Absent;
        if (got != NVAPI_OK || handle == nullptr) {
            LogLine(L"[cuda] NVIDIA's driver would not say which profile covers %s (%d)", exePath.c_str(), got);
            return CudaLookup::Failed;
        }
        NVDRS_PROFILE_V1 info;
        std::memset(&info, 0, sizeof info);
        info.version = MAKE_NVAPI_VERSION(NVDRS_PROFILE_V1, 1);
        const NvAPI_Status told = Api().profileInfo(s->h, handle, &info);
        if (told != NVAPI_OK) {
            LogLine(L"[cuda] a profile covering %s could not be read (%d)", exePath.c_str(), told);
            return CudaLookup::Failed;
        }
        p.profileName = FromUnicodeString(info.profileName);
        p.appEntry = FromUnicodeString(app->appName);
        p.isPredefined = info.isPredefined != 0;
        // R5-1: reported, not judged. What to do with an entry holding more than our own setting is
        // RestoreCudaForRow's decision, in gpu_cuda.h, where the unit suite reaches it.
        p.numSettings = info.numOfSettings;
        bool complete = false;
        p.otherApps = OtherAppCountOf(s, handle, p.appEntry, info.numOfApps, complete);
        p.appsComplete = complete;
        return CudaLookup::Found;
    };

    // 🔴 R5-2: WHAT THE DRIVER SAYS ABOUT A PROFILE ASKED FOR BY NAME, AND NOTHING ELSE. It exists so the
    // ADOPT ruling can live in gpu_cuda.h (CudaAdoptRefusal) instead of inside createProfileForExe below,
    // where no test could reach it. `otherApps` is every application entry it covers - the caller only asks
    // after the lookup for the executable answered Absent, so none of them can be ours.
    ops.findProfileByName = [s](const std::wstring& profileName, CudaProfile& p) {
        p = CudaProfile();
        if (!s->open || profileName.empty()) return CudaLookup::Failed;
        NvAPI_UnicodeString buf;
        ToUnicodeString(buf, profileName);
        NvDRSProfileHandle handle = nullptr;
        const NvAPI_Status got = Api().findProfile(s->h, buf, &handle);
        if (got == NVAPI_PROFILE_NOT_FOUND) return CudaLookup::Absent;
        if (got != NVAPI_OK || handle == nullptr) {
            LogLine(L"[cuda] NVIDIA's driver would not say whether it has a profile called %s (%d)",
                    profileName.c_str(), got);
            return CudaLookup::Failed;
        }
        NVDRS_PROFILE_V1 info;
        std::memset(&info, 0, sizeof info);
        info.version = MAKE_NVAPI_VERSION(NVDRS_PROFILE_V1, 1);
        const NvAPI_Status told = Api().profileInfo(s->h, handle, &info);
        if (told != NVAPI_OK) {
            LogLine(L"[cuda] the profile called %s could not be read (%d)", profileName.c_str(), told);
            return CudaLookup::Failed;
        }
        p.profileName = FromUnicodeString(info.profileName);
        p.isPredefined = info.isPredefined != 0;
        p.numSettings = info.numOfSettings;
        bool complete = false;
        // NOTHING IS EXCLUDED FROM THE COUNT: an entry of ours is not in it yet, by construction.
        p.otherApps = OtherAppCountOf(s, handle, std::wstring(), info.numOfApps, complete);
        p.appsComplete = complete;
        return CudaLookup::Found;
    };

    ops.readSetting = [s](const std::wstring& profileName, std::wstring& value) {
        value.clear();
        const NvDRSProfileHandle handle = ProfileByName(s, profileName);
        if (handle == nullptr) return CudaRead::Failed;
        std::unique_ptr<NVDRS_SETTING_V1> setting(new NVDRS_SETTING_V1());
        std::memset(setting.get(), 0, sizeof(NVDRS_SETTING_V1));
        setting->version = MAKE_NVAPI_VERSION(NVDRS_SETTING_V1, 1);
        const NvAPI_Status got = Api().getSetting(s->h, handle, kCudaExcludedGpus, setting.get());
        // THE ONE ANSWER THAT MEANS "THERE IS NONE" IS THE DRIVER SAYING SO. Every other unhappy answer -
        // a profile that could not be opened, a refused read, a setting that is not the wide string this
        // feature knows how to read - is Failed, and Failed never writes.
        if (got == NVAPI_SETTING_NOT_FOUND) return CudaRead::Absent;
        if (got != NVAPI_OK) return CudaRead::Failed;
        if (setting->settingType != kWideStringType) return CudaRead::Failed;
        value = FromUnicodeString(setting->wszCurrentValue);
        return CudaRead::Value;
    };

    ops.writeSetting = [s](const std::wstring& profileName, bool clear, const std::wstring& value) {
        const NvDRSProfileHandle handle = ProfileByName(s, profileName);
        if (handle == nullptr) return false;
        if (clear) {
            const NvAPI_Status d = Api().deleteSetting(s->h, handle, kCudaExcludedGpus);
            return d == NVAPI_OK || d == NVAPI_SETTING_NOT_FOUND;   // already absent is the wanted state
        }
        std::unique_ptr<NVDRS_SETTING_V1> setting(new NVDRS_SETTING_V1());
        std::memset(setting.get(), 0, sizeof(NVDRS_SETTING_V1));
        setting->version = MAKE_NVAPI_VERSION(NVDRS_SETTING_V1, 1);
        setting->settingId = kCudaExcludedGpus;
        setting->settingType = kWideStringType;
        ToUnicodeString(setting->wszCurrentValue, value);
        return Api().setSetting(s->h, handle, setting.get()) == NVAPI_OK;
    };

    ops.createProfileForExe = [s](const std::wstring& exePath, const std::wstring& adoptName, CudaProfile& made) {
        made = CudaProfile();
        if (!s->open) return CudaCreate::Failed;
        // One profile per application path, named so a user meeting it in NVIDIA's own Control Panel can
        // tell where it came from and what it is for. The name is built in ONE place (gpu_cuda.h), because
        // it is also the test that decides whether this product may ever delete a profile again.
        //
        // 🔴 R6-12: AN ADOPTION OPENS THE NAME THE CALLER RULED ON, NOT ONE RECOMPUTED HERE. [M] probe
        // S23: NvAPI_DRS_FindProfileByName is CASE-SENSITIVE, and ProfileByName below is that call. The
        // caller found this entry by the name its RECORD stores; recomputing one from today's path - which
        // Windows may spell in any case - would miss it here and make a SECOND entry differing only in
        // case. Empty means adopt nothing, and the computed name is then the only one there is.
        const bool reuseNamed = !adoptName.empty();
        const std::wstring profileName = reuseNamed ? adoptName : CudaProfileNameFor(exePath);
        NvDRSProfileHandle handle = ProfileByName(s, profileName);
        // 🔴 E1: AN ENTRY ALREADY CARRYING THIS NAME IS ONLY ADOPTED WHEN THE CALLER SAYS A RECORD LINE
        // NAMES IT. An earlier run that died between making the profile and writing the setting leaves
        // exactly one, and so could a user who typed the name themselves - and the two are indistinguishable
        // from the name alone. Without a line claiming it, NOTHING here is changed: adding our application
        // entry to an entry we do not own is the very write E1 exists to prevent.
        bool created = false;
        size_t others = 0;
        bool othersComplete = true;
        bool predefined = false;
        size_t settings = 0;
        if (handle == nullptr) {
            NVDRS_PROFILE_V1 info;
            std::memset(&info, 0, sizeof info);
            info.version = MAKE_NVAPI_VERSION(NVDRS_PROFILE_V1, 1);
            ToUnicodeString(info.profileName, profileName);
            if (Api().createProfile(s->h, &info, &handle) != NVAPI_OK || handle == nullptr)
                return CudaCreate::Failed;
            created = true;
        } else if (!reuseNamed) {
            return CudaCreate::NameTaken;
        } else {
            // 🔴 R5-2: AN ADOPTION IS MEASURED HERE TOO, NOT ASSUMED. The caller has already ruled on what
            // findProfileByName said (CudaAdoptRefusal, gpu_cuda.h) - this is the second guard over the same
            // decision, taken against the handle actually about to be written, and it is what stops a
            // profile that gained a member between the two calls from being written on as ours.
            NVDRS_PROFILE_V1 info;
            std::memset(&info, 0, sizeof info);
            info.version = MAKE_NVAPI_VERSION(NVDRS_PROFILE_V1, 1);
            if (Api().profileInfo(s->h, handle, &info) != NVAPI_OK) return CudaCreate::Failed;
            predefined = info.isPredefined != 0;
            settings = info.numOfSettings;
            bool complete = false;
            others = OtherAppCountOf(s, handle, std::wstring(), info.numOfApps, complete);
            othersComplete = complete;
            if (predefined || !othersComplete || others != 0) {
                made.profileName = FromUnicodeString(info.profileName);
                made.isPredefined = predefined;
                made.otherApps = othersComplete ? others : 0;
                made.appsComplete = othersComplete;
                made.numSettings = settings;
                return CudaCreate::NameTaken;
            }
        }
        const std::wstring appKey = CudaAppKeyFor(exePath);
        std::unique_ptr<NVDRS_APPLICATION_V4> app(new NVDRS_APPLICATION_V4());
        std::memset(app.get(), 0, sizeof(NVDRS_APPLICATION_V4));
        app->version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
        app->isPredefined = 0;
        ToUnicodeString(app->appName, appKey);
        ToUnicodeString(app->userFriendlyName, BaseName(exePath));
        const NvAPI_Status c = Api().createApp(s->h, handle, app.get());
        // 🔴 `made` IS FILLED EVEN WHEN THIS FAILS, AND THAT IS WHAT MAKES E7 TESTABLE. A profile made
        // moments ago has to go again when the application would not go into it - otherwise an empty
        // "Game Optimizer - <path>" sits in the open session and the NEXT row's whole-session save commits
        // it, an entry in NVIDIA's Control Panel that nothing records and nothing will ever take away.
        // The DECISION to take it away is WriteCudaForRow's, in gpu_cuda.h, where the unit suite reaches
        // it; this call only reports what it did, which is all it can be tested on.
        made.profileName = profileName;
        made.appEntry = appKey;
        made.isPredefined = predefined;
        made.createdNow = created;
        // 🔴 R5-2: WHAT WAS MEASURED, NEVER A CONSTANT. This line read `made.otherApps = 0;` unconditionally
        // and that was the round-4 HIGH: a profile another program had joined was reported as covering
        // nobody else. A profile this call CREATED really does cover nothing but the entry just put in it;
        // an adopted one carries the count the branch above took from the driver.
        made.otherApps = created ? 0 : others;
        made.appsComplete = created ? true : othersComplete;
        made.numSettings = settings;
        // [M] -167 arrives when ANOTHER profile already claims this executable's BASE NAME, and a full
        // path is no escape. The caller reaches this only when the lookup found no profile at all, so
        // there is no entry of ours to edit instead and the row is refused and named.
        if (c == NVAPI_EXECUTABLE_ALREADY_IN_USE) return CudaCreate::AlreadyInUse;
        if (c != NVAPI_OK) return CudaCreate::Failed;
        return CudaCreate::Created;
    };

    ops.deleteProfile = [s](const std::wstring& profileName, const std::wstring& appEntry) {
        return DeleteOwnProfile(s, profileName, appEntry);
    };

    // 🔴 R6-1: THE IDS, NOT A COUNT. What they mean is JudgeCudaSettings' decision in gpu_cuda.h.
    ops.listSettingIds = [s](const std::wstring& profileName, std::vector<unsigned long>& ids) {
        ids.clear();
        const NvDRSProfileHandle handle = ProfileByName(s, profileName);
        if (handle == nullptr) return false;
        return SettingIdsOf(s, handle, ids);
    };

    ops.save = [s]() {
        if (!s->open) return false;
        return Api().saveSettings(s->h) == NVAPI_OK;
    };

    ops.close = [s]() {
        // ONLY THE SESSION. NvAPI_Unload is never called - see the top of this file.
        if (!s->open) return;
        s->open = false;
        Api().destroySession(s->h);
        s->h = nullptr;
    };
    // 🔴 R6-2, AND IT IS THE LAST THING THIS FUNCTION DOES ON PURPOSE. Every lambda above is written as
    // though its entry points resolved; this takes away the ones whose entry points did not. Until round 6
    // the delete lambda was installed unconditionally, so `ops.deleteProfile == nullptr` - the condition
    // PlanCudaForRow refuses NoWayToUndo on, and which the unit suite exercises through its fake's
    // noDelete flag - was unreachable on every real driver there is. The decision is ONE function in
    // gpu_cuda.h so a test can hold it to account; this is its only caller.
    ApplyCudaDriverCalls(ops, CallsOf(api));
    return ops;
}

// ---- The record on disk ------------------------------------------------------------------------
//
// ONE FILE, IN THE CONFIG FOLDER BESIDE config.ini, one line per application (gpu_cuda.h, E2). Every
// write goes through ReplaceTextFile - the whole file to a temporary, then moved over the record in one
// step - exactly as the .reg is written, so what is on disk is always a complete record or the previous
// one. Nothing is ever appended in place.
namespace {

// Local time, as a person reading the file would write it. The record is a file people open.
std::wstring CudaStampNow() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[32];
    swprintf_s(buf, 32, L"%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond);
    return buf;
}

// 🔴 E6: THE PER-RUN FILES v0.5.8's FIRST TWO ROUNDS WROTE. Each gpu-cuda-before-<stamp>.txt is a record
// of changes this product really made, in a format this code no longer reads - and "a format I do not
// understand" is not "nothing was ever changed here". Reading it as the second would leave an application
// pinned to a card in silence, which is the whole of E6. One of them makes the record UNREADABLE, which is
// said out loud; the file itself is never parsed and never deleted.
// `couldLook` is false when the folder would not even be listed, which is the same answer for the same
// reason: nobody knows what is in there.
bool LegacyCudaRecordPresent(const std::wstring& dir, bool& couldLook) {
    couldLook = true;
    WIN32_FIND_DATAW fd;
    const HANDLE f = FindFirstFileW((dir + L"\\gpu-cuda-before-*.txt").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD gle = GetLastError();
        couldLook = gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND;
        return false;
    }
    bool any = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) any = true;
    } while (!any && FindNextFileW(f, &fd));
    FindClose(f);
    return any;
}

}  // namespace

CudaRecord ReadCudaRecord(const std::wstring& dir) {
    CudaRecord out;
    if (dir.empty()) {
        out.state = CudaRecordState::Unreadable;
        return out;
    }
    out.path = dir + L"\\" + CudaRecordFileName();
    bool couldLook = true;
    if (LegacyCudaRecordPresent(dir, couldLook)) {
        out.state = CudaRecordState::Unreadable;
        LogLine(L"[cuda] %s still holds a gpu-cuda-before-*.txt record in the older per-run format; the CUDA "
                L"record is reported unreadable rather than empty", dir.c_str());
        return out;
    }
    if (!couldLook) {
        out.state = CudaRecordState::Unreadable;
        LogLine(L"[cuda] the CUDA records in %s could not be listed", dir.c_str());
        return out;
    }
    // "IT IS NOT THERE" AND "IT WOULD NOT OPEN" ARE OPPOSITE FACTS ABOUT THE SAME FAILED READ, and only
    // the first one means there is nothing to put back (util.h, FileReadResult, for the same rule).
    if (GetFileAttributesW(out.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD gle = GetLastError();
        if (gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND) {
            out.state = CudaRecordState::Missing;
            return out;
        }
        out.state = CudaRecordState::Unreadable;
        LogLine(L"[cuda] the CUDA record %s could not be looked at (gle=%lu)", out.path.c_str(), gle);
        return out;
    }
    std::wstring text;
    if (!ReadRestoreFileText(out.path, text)) {
        out.state = CudaRecordState::Unreadable;
        LogLine(L"[cuda] the CUDA record %s could not be read", out.path.c_str());
        return out;
    }
    if (!ParseCudaRecordFile(text, out.rows)) {
        out.rows.clear();
        out.state = CudaRecordState::Unreadable;
        LogLine(L"[cuda] the CUDA record %s is not in the format this version writes", out.path.c_str());
        return out;
    }
    out.state = CudaRecordState::Ok;
    return out;
}

bool SetCudaRecordRows(CudaRecord& record, const std::vector<CudaRecordRow>& rows) {
    if (record.path.empty()) return false;
    // 🔴 NOTHING IS WRITTEN OVER A RECORD NOBODY COULD READ. Rewriting it would destroy whatever it held -
    // which may be the only note of a change this product made - so it is only ever written when it was
    // read whole, or when it was provably not there at all.
    if (record.state == CudaRecordState::Unreadable) return false;
    if (!ReplaceTextFile(record.path, FormatCudaRecordFile(rows))) return false;
    record.rows = rows;
    record.state = CudaRecordState::Ok;   // a Missing record exists now
    return true;
}

bool SaveCudaRecordRow(CudaRecord& record, CudaRecordRow row) {
    if (record.state == CudaRecordState::Unreadable) return false;
    if (row.when.empty()) row.when = CudaStampNow();
    if (!CudaRecordRowIsWritable(row)) return false;
    return SetCudaRecordRows(record, CudaRowsWith(record.rows, row));
}

bool ForgetCudaRecordRow(CudaRecord& record, const std::wstring& appEntry) {
    if (record.path.empty() || record.state != CudaRecordState::Ok) return false;
    const std::vector<CudaRecordRow> kept = CudaRowsWithout(record.rows, appEntry);
    if (kept.size() == record.rows.size()) return true;   // there was no line for it: nothing to take out
    if (!SetCudaRecordRows(record, kept)) {
        LogLine(L"[cuda] the CUDA record %s could not be rewritten after a restore", record.path.c_str());
        return false;
    }
    return true;
}

}  // namespace cd
