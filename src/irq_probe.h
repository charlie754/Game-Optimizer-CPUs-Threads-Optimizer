// Game Optimizer - interrupt and DPC bench, the IMPURE half.
//
// Everything here touches the operating system: the PDH per-processor counters, the
// cfgmgr32 device enumeration and the registry reads under each device's Device Parameters
// key. Included by irqwindow.cpp and by NO test - the decisions and the wording live in
// irq_policy.h, which is header-only and which the unit harness links with no build-script
// edit at all.
//
// THIS TRANSLATION UNIT WRITES NOTHING. Every registry handle it opens is opened with
// KEY_QUERY_VALUE and nothing wider, every device is opened read-only, and there is no
// elevation, no device restart and no RegSetValueExW anywhere in it. [M] Both halves were
// measured working on a NON-elevated token on 2026-09-08: Get-Counter over
// \Processor(*)\% DPC Time returned 33 instances with IsInRole(Administrator) false, and the
// same token read MessageNumberLimit out of a PCI device's Interrupt Management subtree.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

#include "irq_policy.h"
#include "topology.h"

namespace cd {

// One present PCI device and everything this page read about it. Absence and unreadability
// are separate flags on purpose, in both directions: settings_warning.h states the rule for
// the CPU Sets readback and it is the same rule here - a value that could not be read must
// never report as "not set", and must never report as set either.
struct IrqDevice {
    std::wstring instanceId;      // full PnP instance id, case preserved
    std::wstring friendlyName;    // CM_DRP_FRIENDLYNAME, else CM_DRP_DEVICEDESC
    std::wstring className;       // CM_DRP_CLASS, e.g. "Display", "Media"
    bool  hasProblemCode = false;
    ULONG problemCode = 0;

    // The Device Parameters key itself could not be opened, so NOTHING below is known.
    bool keyReadFailed = false;

    // Interrupt Management\Affinity Policy - the static policy, the one a .reg writes.
    bool  affinityPolicyKeyExists = false;
    bool  devicePolicyPresent = false;      DWORD devicePolicy = 0;
    bool  overridePresent = false;          std::vector<BYTE> overrideBytes;
    bool  overrideDecoded = false;          ULONG_PTR overrideMask = 0;

    // Interrupt Management\MessageSignaledInterruptProperties\MessageNumberLimit.
    // [M] Both GPUs on the reference machine ship MessageNumberLimit = 1, i.e. a single MSI
    // vector, which cannot spread across processors at all.
    bool  messageNumberLimitPresent = false; DWORD messageNumberLimit = 0;

    // Interrupt Management\Affinity Policy - Temporal - WRITTEN BY WINDOWS, not by any .reg.
    // [M] Found 2026-09-08 on both reference GPUs, one of which has no static policy at all,
    // which is what proves Windows is the author. This is the key that disagreed with the
    // static policy and explains why writing the static one moved nothing.
    bool  temporalKeyExists = false;
    bool  targetGroupPresent = false;       DWORD targetGroup = 0;
    bool  targetSetPresent = false;         DWORD targetSet = 0;
    bool  targetSetDecoded = false;         ULONG_PTR targetSetMask = 0;

    // Folded into the pure classifier in irq_policy.h. Kept beside the raw values rather
    // than replacing them, so the window can show the numbers the sentence was built from.
    IrqPolicyReadout Readout() const {
        IrqPolicyReadout r;
        r.readFailed    = keyReadFailed;
        r.policyPresent = overrideDecoded;
        // PRESENT AND UNDECODABLE IS ITS OWN ANSWER, and this line is what makes it one.
        // overridePresent used to be written by the probe and read NOWHERE, so a value that
        // is really in the registry and that this page cannot decode came out of here looking
        // exactly like a device with no policy at all - and the window then printed "Windows
        // chooses this device's interrupt processors". [M] Council review 2026-09-08, C2/H3.
        r.policyUnreadable = overridePresent && !overrideDecoded;
        r.policyMask    = overrideMask;
        r.targetPresent = targetSetDecoded;
        r.targetMask    = targetSetMask;
        return r;
    }
};

// Present PCI devices only, in the order cfgmgr32 reports them. Returns false and fills
// `error` when the enumeration itself failed; an empty list with a true return means the
// machine really has no present PCI node this page could locate, which is a different thing.
bool IrqEnumerateDevices(std::vector<IrqDevice>& out, std::wstring* error);

// ---- The PDH sampler -------------------------------------------------------
// PdhAddEnglishCounterW, never PdhAddCounterW: localized counter names differ per Windows
// display language and a German machine would silently match nothing.
// "Processor Information", never "Processor": the latter is capped at 64 instances and is
// blind to processor groups.
struct IrqSampler;

// Returns nullptr and fills `error` when the query or either counter could not be opened -
// PDH_ACCESS_DENIED included, reported rather than hidden. There is no path on which this
// failing produces a screen full of 0.00%.
IrqSampler* IrqSamplerOpen(std::wstring* error);
void        IrqSamplerClose(IrqSampler* s);

// One collection. THE FIRST ONE AFTER OPEN PRIMES THE RATE COUNTERS AND CONTRIBUTES NO
// SAMPLE BY CONTRACT. Returns false when the collection yielded nothing usable; a false
// return is not fatal and the caller may simply collect again.
bool IrqSamplerCollect(IrqSampler* s);

// Collections that actually contributed a sample, i.e. excluding the priming one.
int  IrqSamplerSampleCount(const IrqSampler* s);

// The finished per-processor figures, ascending by logical processor. Only processors that
// contributed at least one valid sample appear; `inGameGroup` is left false for the caller
// to fill through IrqMarkGameGroup.
void IrqSamplerResults(const IrqSampler* s, std::vector<CoreDpcStat>& out);

// The last PDH status this sampler saw, as a sentence, or an empty string while nothing has
// gone wrong. Shown on screen rather than logged away, because a counter that stopped
// returning data must not look like a machine that went quiet.
std::wstring IrqSamplerStatus(const IrqSampler* s);

}  // namespace cd
