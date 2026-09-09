// Game Optimizer - the version string shown in the corner of the settings window.
//
// THE .rc IS THE SINGLE SOURCE OF TRUTH AND THERE IS NO SECOND ONE. Chair ruling: the number
// is never typed into C++ and there is no shared `#define` either. The label is read at run
// time out of the RUNNING MODULE'S OWN version resource, so what it shows is what the binary
// actually reports to Explorer, to the installer and to a bug report. A hardcoded constant -
// even a shared one - is a second representation of the same fact, and two representations
// of one fact drift.
//
// This header holds only the PURE half: turning the two packed DWORDs of a VS_FIXEDFILEINFO
// into the string a human reads. The Win32 half (GetModuleFileNameW / GetFileVersionInfoW /
// VerQueryValueW) lives in settings.cpp, because that is where the window is and because
// nothing here may need version.lib to be unit-tested.
#pragma once

#include <string>

namespace cd {

// Pure. Format the packed file-version fields as the label the window draws.
//
//   ms = (major << 16) | minor          ls = (patch << 16) | build
//
// SHAPE, and it is a judgement call worth writing down: "v0.4.3" for a release, and
// "v0.4.3.1" only when the fourth field is non-zero. The operator's own name for this
// release is "v0.4.3", so a corner label reading "v0.4.3.0" would be four characters of
// noise that match nothing they ever said - while a rebuild that DID move the fourth field
// is exactly the case where the extra digit is the only thing distinguishing two binaries.
//
// ALL ZEROES RETURNS EMPTY, which the caller renders as "draw nothing". A resource that
// reads 0.0.0.0 carries no more information than a failed read, and the rule for a failed
// read is to hide the label: a missing version is harmless, a wrong one is not.
inline std::wstring FormatVersionLabel(unsigned long ms, unsigned long ls) {
    const unsigned major = static_cast<unsigned>((ms >> 16) & 0xFFFFu);
    const unsigned minor = static_cast<unsigned>(ms & 0xFFFFu);
    const unsigned patch = static_cast<unsigned>((ls >> 16) & 0xFFFFu);
    const unsigned build = static_cast<unsigned>(ls & 0xFFFFu);

    if (major == 0 && minor == 0 && patch == 0 && build == 0) return std::wstring();

    std::wstring s = L"v" + std::to_wstring(major) + L"." + std::to_wstring(minor)
                   + L"." + std::to_wstring(patch);
    if (build != 0) s += L"." + std::to_wstring(build);
    return s;
}

}  // namespace cd
