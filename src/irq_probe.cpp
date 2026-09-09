// Game Optimizer - interrupt and DPC bench, the impure half. See irq_probe.h for the
// contract; this file is the operating-system calls only.
//
// NOTHING HERE WRITES. Every registry handle is opened with KEY_QUERY_VALUE, every device
// node is opened RegDisposition_OpenExisting, and there is no RegSetValueExW, no
// RegCreateKeyEx, no ShellExecuteExW and no device restart in this translation unit. That is
// checkable by grep and is meant to be.
//
// ACCESS RIGHTS ARE THE MINIMUM THAT WORKS, the same rule the rest of this product follows:
// KEY_QUERY_VALUE and nothing wider. KEY_ALL_ACCESS appears nowhere in this product and does
// not start here.
//
// NO GUID AND NO DEVPROPKEY IS SPELLED OUT IN THIS FILE, and that is a deliberate simplifi-
// cation of the design it was built from. The design proposed CM_Get_DevNode_PropertyW with
// hand-transcribed DEVPROPKEY literals, because <initguid.h> would make GUID definition
// depend on include ordering across the whole build (the decision already recorded in
// src\util.cpp). The older CM_Get_DevNode_Registry_PropertyW takes a small integer property
// id instead - CM_DRP_DEVICEDESC, CM_DRP_FRIENDLYNAME, CM_DRP_CLASS - so it needs no GUID at
// all, and a transcription error that cannot be made is better than one that fails safe.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cfgmgr32.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <map>
#include <string>
#include <vector>

#include "irq_probe.h"
#include "util.h"

namespace cd {

namespace {

// ---------------------------------------------------------------------------
// Registry reads. Each one distinguishes ABSENT from UNREADABLE, because the whole point of
// this page is that "there is no policy here" and "I could not look" are different answers.
// ---------------------------------------------------------------------------

// True when the value was read. `missing` is set when it simply is not there, which is the
// ordinary case and is not a failure. Anything else leaves `missing` false and is a failure.
bool ReadDwordValue(HKEY key, const wchar_t* name, DWORD& out, bool& missing) {
    missing = false;
    DWORD type = 0;
    DWORD data = 0;
    DWORD cb = sizeof(data);
    const LSTATUS rc = RegQueryValueExW(key, name, nullptr, &type,
                                        reinterpret_cast<BYTE*>(&data), &cb);
    if (rc == ERROR_FILE_NOT_FOUND) { missing = true; return false; }
    if (rc != ERROR_SUCCESS) return false;
    if (type != REG_DWORD || cb != sizeof(DWORD)) return false;
    out = data;
    return true;
}

bool ReadBinaryValue(HKEY key, const wchar_t* name, std::vector<BYTE>& out, bool& missing) {
    missing = false;
    DWORD type = 0;
    DWORD cb = 0;
    LSTATUS rc = RegQueryValueExW(key, name, nullptr, &type, nullptr, &cb);
    if (rc == ERROR_FILE_NOT_FOUND) { missing = true; return false; }
    if (rc != ERROR_SUCCESS) return false;
    // A pathological size is refused rather than allocated: this value is a processor mask,
    // and nothing legitimate here is larger than a handful of bytes.
    if (cb == 0 || cb > 64) return false;
    out.assign(cb, 0);
    const DWORD allocated = cb;
    rc = RegQueryValueExW(key, name, nullptr, &type, &out[0], &cb);
    // THE VALUE CAN CHANGE BETWEEN THE TWO QUERIES, and this used to trust the second one.
    // A value that shrank to zero bytes in that window returned TRUE holding an EMPTY vector,
    // and the caller below then took &bytes[0] of it - undefined behaviour, and the reason
    // this guard exists. The decision itself is in irq_policy.h so a test can reach it.
    if (!IrqRegSecondReadIsUsable(rc, allocated, cb)) { out.clear(); return false; }
    out.resize(cb);
    return true;
}

// A value that names a processor set, whichever shape Windows stored it in. [M] The temporal
// TargetSet was measured as REG_DWORD on 2026-09-08 and AssignmentSetOverride as REG_BINARY,
// so both shapes are real on one machine and neither may be assumed at a call site.
bool ReadProcessorSetValue(HKEY key, const wchar_t* name, ULONG_PTR& mask, DWORD& raw32,
                           bool& missing, bool& failed) {
    missing = false;
    failed = false;
    DWORD type = 0;
    DWORD cb = 0;
    LSTATUS rc = RegQueryValueExW(key, name, nullptr, &type, nullptr, &cb);
    if (rc == ERROR_FILE_NOT_FOUND) { missing = true; return false; }
    if (rc != ERROR_SUCCESS || cb == 0 || cb > 64) { failed = true; return false; }

    std::vector<BYTE> buf(cb, 0);
    const DWORD allocated = cb;
    rc = RegQueryValueExW(key, name, nullptr, &type, &buf[0], &cb);
    // The same two-query race as ReadBinaryValue above. The resize matters as well as the
    // refusal: without it a value that shrank would be decoded at its ALLOCATED length, with
    // the stale tail of the buffer read as part of the mask.
    if (!IrqRegSecondReadIsUsable(rc, allocated, cb)) { failed = true; return false; }
    buf.resize(cb);

    if (type == REG_DWORD && cb == sizeof(DWORD)) {
        DWORD v = 0;
        memcpy(&v, &buf[0], sizeof(v));
        raw32 = v;
        mask = IrqTargetSetToMask(v);
        return true;
    }
    // REG_BINARY, and the byte order is decided by the pure decoder rather than by this
    // machine's storage order - see IrqRegBytesToMask.
    if (IrqRegBytesToMask(buf, mask)) {
        raw32 = (DWORD)(mask & 0xFFFFFFFFull);
        return true;
    }
    failed = true;
    return false;
}

// Opens a subkey read-only. Distinguishes "not there" from "could not open".
bool OpenSubkeyForRead(HKEY parent, const wchar_t* path, HKEY& out, bool& missing) {
    missing = false;
    const LSTATUS rc = RegOpenKeyExW(parent, path, 0, KEY_QUERY_VALUE, &out);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) { missing = true; return false; }
    return rc == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// cfgmgr32 device properties
// ---------------------------------------------------------------------------

// CM_Get_DevNode_Registry_PropertyW into a wstring. Empty on any failure, which the caller
// treats as "not identified" rather than inventing a name.
std::wstring DevNodeStringProperty(DEVINST devInst, ULONG property) {
    ULONG type = 0;
    ULONG cb = 0;
    CONFIGRET cr = CM_Get_DevNode_Registry_PropertyW(devInst, property, &type, nullptr, &cb, 0);
    if (cr != CR_BUFFER_SMALL || cb == 0 || cb > 64 * 1024) return std::wstring();
    std::vector<BYTE> buf(cb, 0);
    cr = CM_Get_DevNode_Registry_PropertyW(devInst, property, &type, &buf[0], &cb, 0);
    if (cr != CR_SUCCESS) return std::wstring();
    if (type != REG_SZ && type != REG_EXPAND_SZ) return std::wstring();
    const wchar_t* p = reinterpret_cast<const wchar_t*>(&buf[0]);
    // The buffer is not guaranteed to be terminated inside its own length, so bound the scan
    // by the length that came back rather than trusting the string.
    const size_t maxChars = buf.size() / sizeof(wchar_t);
    size_t n = 0;
    while (n < maxChars && p[n] != L'\0') ++n;
    return std::wstring(p, n);
}

// ---------------------------------------------------------------------------
// The PDH sampler
// ---------------------------------------------------------------------------

// English counter names, ALWAYS: PdhAddCounterW would take localized names, so a machine
// whose Windows display language is not English would silently match no counter at all.
const wchar_t kDpcCounterPath[] = L"\\Processor Information(*)\\% DPC Time";
const wchar_t kIsrCounterPath[] = L"\\Processor Information(*)\\% Interrupt Time";

struct Accumulator {
    int    samples = 0;
    double sumDpc = 0.0, minDpc = 0.0, maxDpc = 0.0;
    double sumIsr = 0.0, minIsr = 0.0, maxIsr = 0.0;
};

// "0,3" is processor 3 of group 0. "_Total" and "0,_Total" are rollups and are rejected -
// letting one through would put a machine-wide figure in a per-processor column.
//
// A bare decimal with no comma is accepted as group 0. That form belongs to the older
// "Processor" object rather than to "Processor Information", and accepting it costs nothing
// while refusing it would empty the whole page on a machine that reports it.
bool ParseProcessorInstance(const std::wstring& name, ULONG& outLp) {
    if (name.empty()) return false;
    const size_t comma = name.find(L',');
    std::wstring groupPart, cpuPart;
    if (comma == std::wstring::npos) {
        groupPart = L"0";
        cpuPart = name;
    } else {
        groupPart = name.substr(0, comma);
        cpuPart = name.substr(comma + 1);
    }
    unsigned long group = 0, cpu = 0;
    if (!ParseUlongW(groupPart, group)) return false;
    if (!ParseUlongW(cpuPart, cpu)) return false;
    // One processor group only. The bench refuses a multi-group machine up front, so a
    // group-1 instance arriving here would be a contradiction, not data.
    if (group != 0) return false;
    if (cpu >= kIrqMaxLogicalProcessors) return false;
    outLp = (ULONG)cpu;
    return true;
}

}  // namespace

struct IrqSampler {
    PDH_HQUERY   query = nullptr;
    PDH_HCOUNTER dpc = nullptr;
    PDH_HCOUNTER isr = nullptr;
    int collects = 0;   // successful collections, INCLUDING the priming one
    int samples = 0;    // collections that contributed at least one processor reading
    std::map<ULONG, Accumulator> acc;
    std::wstring status;
};

namespace {

// Reads one wildcard counter into lp -> value. Only items whose own CStatus is valid are
// stored: A FAILED READ IS NOT 0.0%, and a processor missing from this map contributes
// nothing to its accumulator rather than dragging its mean down.
bool ReadCounterArray(PDH_HCOUNTER counter, std::map<ULONG, double>& out, PDH_STATUS& lastStatus) {
    out.clear();
    DWORD cb = 0;
    DWORD count = 0;
    lastStatus = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &cb, &count, nullptr);
    if (lastStatus != PDH_MORE_DATA) return false;
    if (cb == 0 || count == 0) return false;

    std::vector<BYTE> buf(cb, 0);
    PDH_FMT_COUNTERVALUE_ITEM_W* items =
        reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(&buf[0]);
    lastStatus = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &cb, &count, items);
    if (lastStatus != ERROR_SUCCESS) return false;

    for (DWORD i = 0; i < count; ++i) {
        if (!items[i].szName) continue;
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
            items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
            continue;
        ULONG lp = 0;
        if (!ParseProcessorInstance(items[i].szName, lp)) continue;
        out[lp] = items[i].FmtValue.doubleValue;
    }
    return !out.empty();
}

std::wstring PdhFailureSentence(const wchar_t* what, PDH_STATUS st) {
    wchar_t buf[192];
    swprintf(buf, 192,
             L"The per-processor counters could not be read (%s, PDH status 0x%08lX).",
             what, (unsigned long)st);
    return std::wstring(buf);
}

}  // namespace

IrqSampler* IrqSamplerOpen(std::wstring* error) {
    IrqSampler* s = new IrqSampler();
    PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &s->query);
    if (st != ERROR_SUCCESS) {
        if (error) *error = PdhFailureSentence(L"opening the query", st);
        LogLine(L"[irq] PdhOpenQueryW failed, status=0x%08lX", (unsigned long)st);
        delete s;
        return nullptr;
    }
    st = PdhAddEnglishCounterW(s->query, kDpcCounterPath, 0, &s->dpc);
    if (st != ERROR_SUCCESS) {
        if (error) *error = PdhFailureSentence(L"adding % DPC Time", st);
        LogLine(L"[irq] PdhAddEnglishCounterW(%% DPC Time) failed, status=0x%08lX",
                (unsigned long)st);
        PdhCloseQuery(s->query);
        delete s;
        return nullptr;
    }
    st = PdhAddEnglishCounterW(s->query, kIsrCounterPath, 0, &s->isr);
    if (st != ERROR_SUCCESS) {
        if (error) *error = PdhFailureSentence(L"adding % Interrupt Time", st);
        LogLine(L"[irq] PdhAddEnglishCounterW(%% Interrupt Time) failed, status=0x%08lX",
                (unsigned long)st);
        PdhCloseQuery(s->query);
        delete s;
        return nullptr;
    }
    return s;
}

void IrqSamplerClose(IrqSampler* s) {
    if (!s) return;
    if (s->query) PdhCloseQuery(s->query);
    delete s;
}

bool IrqSamplerCollect(IrqSampler* s) {
    if (!s || !s->query) return false;
    const PDH_STATUS st = PdhCollectQueryData(s->query);
    if (st != ERROR_SUCCESS) {
        s->status = PdhFailureSentence(L"collecting", st);
        return false;
    }
    ++s->collects;
    // THE FIRST COLLECTION PRIMES THE RATE COUNTERS AND CONTRIBUTES NO SAMPLE BY CONTRACT.
    // A rate counter has nothing to compare against on its first read, so the value it would
    // report is not a measurement of anything.
    if (s->collects == 1) return true;

    std::map<ULONG, double> dpc, isr;
    PDH_STATUS dpcStatus = ERROR_SUCCESS, isrStatus = ERROR_SUCCESS;
    const bool haveDpc = ReadCounterArray(s->dpc, dpc, dpcStatus);
    const bool haveIsr = ReadCounterArray(s->isr, isr, isrStatus);
    if (!haveDpc || !haveIsr) {
        s->status = PdhFailureSentence(haveDpc ? L"% Interrupt Time" : L"% DPC Time",
                                       haveDpc ? isrStatus : dpcStatus);
        return false;
    }

    int updated = 0;
    for (std::map<ULONG, double>::const_iterator it = dpc.begin(); it != dpc.end(); ++it) {
        const std::map<ULONG, double>::const_iterator other = isr.find(it->first);
        // A SAMPLE IS A PAIR. If either counter failed for this processor in this collection
        // the pair is discarded, so a mean is never the average of two different sample sets.
        if (other == isr.end()) continue;
        Accumulator& a = s->acc[it->first];
        const double d = it->second;
        const double i = other->second;
        if (a.samples == 0) {
            a.minDpc = a.maxDpc = d;
            a.minIsr = a.maxIsr = i;
        } else {
            if (d < a.minDpc) a.minDpc = d;
            if (d > a.maxDpc) a.maxDpc = d;
            if (i < a.minIsr) a.minIsr = i;
            if (i > a.maxIsr) a.maxIsr = i;
        }
        a.sumDpc += d;
        a.sumIsr += i;
        ++a.samples;
        ++updated;
    }
    if (updated == 0) return false;
    ++s->samples;
    s->status.clear();
    return true;
}

int IrqSamplerSampleCount(const IrqSampler* s) { return s ? s->samples : 0; }

void IrqSamplerResults(const IrqSampler* s, std::vector<CoreDpcStat>& out) {
    out.clear();
    if (!s) return;
    for (std::map<ULONG, Accumulator>::const_iterator it = s->acc.begin(); it != s->acc.end();
         ++it) {
        const Accumulator& a = it->second;
        if (a.samples <= 0) continue;
        CoreDpcStat c;
        c.lp = it->first;
        c.samples = a.samples;
        c.meanDpcPct = a.sumDpc / a.samples;
        c.minDpcPct = a.minDpc;
        c.maxDpcPct = a.maxDpc;
        c.meanIsrPct = a.sumIsr / a.samples;
        c.minIsrPct = a.minIsr;
        c.maxIsrPct = a.maxIsr;
        out.push_back(c);
    }
}

std::wstring IrqSamplerStatus(const IrqSampler* s) {
    return s ? s->status : std::wstring();
}

// ---------------------------------------------------------------------------
// Device enumeration
// ---------------------------------------------------------------------------

bool IrqEnumerateDevices(std::vector<IrqDevice>& out, std::wstring* error) {
    out.clear();

    const ULONG filter = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    ULONG len = 0;
    CONFIGRET cr = CM_Get_Device_ID_List_SizeW(&len, L"PCI", filter);
    if (cr != CR_SUCCESS || len < 2) {
        if (error) *error = L"Windows would not list this machine's PCI devices.";
        LogLine(L"[irq] CM_Get_Device_ID_List_SizeW failed, cr=%lu len=%lu",
                (unsigned long)cr, (unsigned long)len);
        return false;
    }
    std::vector<wchar_t> ids(len, L'\0');
    cr = CM_Get_Device_ID_ListW(L"PCI", &ids[0], len, filter);
    if (cr != CR_SUCCESS) {
        if (error) *error = L"Windows would not list this machine's PCI devices.";
        LogLine(L"[irq] CM_Get_Device_ID_ListW failed, cr=%lu", (unsigned long)cr);
        return false;
    }

    // A REG_MULTI_SZ-shaped buffer: consecutive strings, terminated by an empty one.
    for (size_t i = 0; i < ids.size() && ids[i] != L'\0';) {
        const std::wstring id(&ids[i]);
        i += id.size() + 1;
        if (id.empty()) continue;

        DEVINST devInst = 0;
        cr = CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(id.c_str()),
                                CM_LOCATE_DEVNODE_NORMAL);
        if (cr != CR_SUCCESS) continue;

        IrqDevice d;
        d.instanceId = id;
        d.friendlyName = DevNodeStringProperty(devInst, CM_DRP_FRIENDLYNAME);
        if (d.friendlyName.empty())
            d.friendlyName = DevNodeStringProperty(devInst, CM_DRP_DEVICEDESC);
        d.className = DevNodeStringProperty(devInst, CM_DRP_CLASS);

        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, devInst, 0) == CR_SUCCESS) {
            if ((status & DN_HAS_PROBLEM) != 0) {
                d.hasProblemCode = true;
                d.problemCode = problem;
            }
        }

        HKEY hDev = nullptr;
        // CM_REGISTRY_HARDWARE is the device's own "Device Parameters" key, and
        // RegDisposition_OpenExisting means this call cannot create it. The Enum path is
        // NEVER composed from an enumerated string here, which removes that whole class of
        // hazard; the only path text below is a fixed relative literal.
        cr = CM_Open_DevNode_Key(devInst, KEY_QUERY_VALUE, 0, RegDisposition_OpenExisting,
                                 &hDev, CM_REGISTRY_HARDWARE);
        if (cr != CR_SUCCESS) {
            // A device with no Device Parameters key at all is an ordinary state and is NOT
            // an unreadable one. Anything else is a failure and is reported as such.
            if (cr != CR_NO_SUCH_REGISTRY_KEY && cr != CR_NO_SUCH_VALUE)
                d.keyReadFailed = true;
            out.push_back(d);
            continue;
        }

        bool missing = false;
        HKEY hPolicy = nullptr;
        if (OpenSubkeyForRead(hDev, L"Interrupt Management\\Affinity Policy", hPolicy, missing)) {
            d.affinityPolicyKeyExists = true;
            DWORD dw = 0;
            if (ReadDwordValue(hPolicy, L"DevicePolicy", dw, missing)) {
                d.devicePolicyPresent = true;
                d.devicePolicy = dw;
            } else if (!missing) {
                d.keyReadFailed = true;
            }
            std::vector<BYTE> bytes;
            if (ReadBinaryValue(hPolicy, L"AssignmentSetOverride", bytes, missing)) {
                d.overridePresent = true;
                d.overrideBytes = bytes;
                // THE VECTOR FORM, NEVER &bytes[0]. ReadBinaryValue cannot hand back an
                // empty buffer any more, and this call site does not depend on that being
                // true: an empty vector is refused inside the decoder instead.
                d.overrideDecoded = IrqRegBytesToMask(bytes, d.overrideMask);
            } else if (!missing) {
                d.keyReadFailed = true;
            }
            RegCloseKey(hPolicy);
        } else if (!missing) {
            d.keyReadFailed = true;
        }

        HKEY hMsi = nullptr;
        if (OpenSubkeyForRead(hDev,
                              L"Interrupt Management\\MessageSignaledInterruptProperties",
                              hMsi, missing)) {
            DWORD dw = 0;
            if (ReadDwordValue(hMsi, L"MessageNumberLimit", dw, missing)) {
                d.messageNumberLimitPresent = true;
                d.messageNumberLimit = dw;
            } else if (!missing) {
                d.keyReadFailed = true;
            }
            RegCloseKey(hMsi);
        } else if (!missing) {
            d.keyReadFailed = true;
        }

        HKEY hTemporal = nullptr;
        if (OpenSubkeyForRead(hDev, L"Interrupt Management\\Affinity Policy - Temporal",
                              hTemporal, missing)) {
            d.temporalKeyExists = true;
            DWORD dw = 0;
            if (ReadDwordValue(hTemporal, L"TargetGroup", dw, missing)) {
                d.targetGroupPresent = true;
                d.targetGroup = dw;
            } else if (!missing) {
                d.keyReadFailed = true;
            }
            ULONG_PTR mask = 0;
            DWORD raw = 0;
            bool failed = false;
            if (ReadProcessorSetValue(hTemporal, L"TargetSet", mask, raw, missing, failed)) {
                d.targetSetPresent = true;
                d.targetSet = raw;
                d.targetSetDecoded = true;
                d.targetSetMask = mask;
            } else if (failed) {
                d.targetSetPresent = true;
                d.keyReadFailed = true;
            }
            RegCloseKey(hTemporal);
        } else if (!missing) {
            d.keyReadFailed = true;
        }

        RegCloseKey(hDev);
        out.push_back(d);
    }

    LogLine(L"[irq] enumerated %d present PCI devices", (int)out.size());
    return true;
}

}  // namespace cd
