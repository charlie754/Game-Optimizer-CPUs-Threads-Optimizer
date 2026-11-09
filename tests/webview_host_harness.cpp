#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "webview_host.h"

namespace gp = Gdiplus;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t kParentClass[] = L"GameOptimizerWebViewHarnessParent";
constexpr int kStripHeight = 76;
constexpr int kPlateWidth = 290;
constexpr int kPlateRightMargin = 13;

struct ReadyState {
    bool called = false;
    bool ok = false;
};

LRESULT CALLBACK ParentProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void Ready(void* user, bool ok) {
    ReadyState* state = static_cast<ReadyState*>(user);
    if (state == nullptr) return;
    state->called = true;
    state->ok = ok;
    std::printf("WEBVIEW_READY ok=%d\n", ok ? 1 : 0);
    std::fflush(stdout);
}

bool PumpUntil(const std::function<bool()>& done, DWORD timeoutMs) {
    const DWORD deadline = GetTickCount() + timeoutMs;
    for (;;) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (done()) return true;
        if (static_cast<LONG>(GetTickCount() - deadline) >= 0) return false;
        MsgWaitForMultipleObjectsEx(0, nullptr, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

void PumpFor(DWORD durationMs) {
    const DWORD deadline = GetTickCount() + durationMs;
    while (static_cast<LONG>(GetTickCount() - deadline) < 0) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return std::wstring();
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (chars <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(chars), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), &out[0], chars) != chars) {
        return std::wstring();
    }
    return out;
}

std::string ReadBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::string();
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

fs::path AppLogPath() {
    wchar_t localAppData[MAX_PATH] = {};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
                                                static_cast<DWORD>(std::size(localAppData)));
    if (count == 0 || count >= std::size(localAppData)) return fs::path();
    return fs::path(localAppData) / L"GameOptimizer" / L"GameOptimizer.log";
}

std::streamoff LogSize() {
    const fs::path path = AppLogPath();
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return 0;
    return static_cast<std::streamoff>(input.tellg());
}

void PrintSponsorLogLines(std::streamoff start) {
    const fs::path path = AppLogPath();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::printf("LOG sponsor log unavailable path=%s\n", path.string().c_str());
        return;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (start < 0 || start > end) start = 0;
    input.seekg(start, std::ios::beg);
    int relevant = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (line.find("[sponsor]") != std::string::npos) {
            std::printf("LOG %s\n", line.c_str());
            ++relevant;
        }
    }
    if (relevant == 0) std::printf("LOG new relevant sponsor lines=0\n");
}

bool Capture(cd::WebSponsor* sponsor, int width, const fs::path& path) {
    const RECT rc{0, 0, width, kStripHeight};
    cd::WebSponsorMove(sponsor, rc);
    std::printf("CAPTURE_BEGIN width=%d path=%s\n", width, path.string().c_str());
    std::fflush(stdout);
    if (!cd::WebSponsorCapturePreviewForTest(sponsor, path.wstring().c_str())) {
        std::printf("CAPTURE_REQUEST_FAIL path=%s\n", path.string().c_str());
        return false;
    }
    bool ok = false;
    if (!PumpUntil([&]() { return cd::WebSponsorCaptureDoneForTest(sponsor, &ok); }, 5000)) {
        std::printf("CAPTURE timeout path=%s\n", path.string().c_str());
        return false;
    }
    std::error_code error;
    const auto bytes = fs::file_size(path, error);
    if (!ok || error) {
        std::printf("CAPTURE FAIL path=%s bytes=0\n", path.string().c_str());
        return false;
    }
    std::printf("CAPTURE path=%s bytes=%llu\n", path.string().c_str(),
                static_cast<unsigned long long>(bytes));
    return true;
}

bool ExecuteScript(cd::WebSponsor* sponsor, const wchar_t* script) {
    if (!cd::WebSponsorExecuteScriptForTest(sponsor, script)) return false;
    bool ok = false;
    if (!PumpUntil([&]() { return cd::WebSponsorScriptDoneForTest(sponsor, &ok); }, 5000)) {
        std::printf("SCRIPT timeout\n");
        return false;
    }
    std::printf("SCRIPT ok=%d\n", ok ? 1 : 0);
    return ok;
}

int PngEncoder(CLSID* out) {
    UINT count = 0, bytes = 0;
    if (gp::GetImageEncodersSize(&count, &bytes) != gp::Ok || bytes == 0) return 0;
    std::vector<BYTE> storage(bytes);
    gp::ImageCodecInfo* codecs = reinterpret_cast<gp::ImageCodecInfo*>(storage.data());
    if (gp::GetImageEncoders(count, bytes, codecs) != gp::Ok) return 0;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            *out = codecs[i].Clsid;
            return 1;
        }
    }
    return 0;
}

bool CropPlate2x(const fs::path& source, int width, const fs::path& destination) {
    gp::Bitmap image(source.wstring().c_str(), FALSE);
    if (image.GetLastStatus() != gp::Ok || image.GetWidth() < static_cast<UINT>(width) ||
        image.GetHeight() < static_cast<UINT>(kStripHeight)) return false;
    const int x = width - kPlateRightMargin - kPlateWidth;
    if (x < 0) return false;
    gp::Bitmap crop(kPlateWidth * 2, kStripHeight * 2, PixelFormat32bppARGB);
    if (crop.GetLastStatus() != gp::Ok) return false;
    gp::Graphics graphics(&crop);
    graphics.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
    if (graphics.DrawImage(&image, gp::Rect(0, 0, kPlateWidth * 2, kStripHeight * 2),
                           x, 0, kPlateWidth, kStripHeight, gp::UnitPixel) != gp::Ok) {
        return false;
    }
    CLSID png{};
    return PngEncoder(&png) != 0 && crop.Save(destination.wstring().c_str(), &png, nullptr) == gp::Ok;
}

bool AssertGeometry(cd::WebSponsor* sponsor, int width, const char* state) {
    const wchar_t script[] =
        L"(function(){"
        L"function b(e){var r=e.getBoundingClientRect();return r;}"
        L"function f(s){throw new Error(s);}"
        L"var h=document.getElementById('h'),s=h&&h.shadowRoot,root=s&&s.querySelector('.goat--strip'),"
        L"p=root&&root.querySelector('.goat-lockup'),k=s&&s.querySelector('.kofi'),badge=p&&p.querySelector('.gc-badge'),"
        L"tag=p&&p.querySelector('.goat-lockup__tag'),vf=p&&p.querySelector('.gc-vf'),"
        L"photo=p&&p.querySelector('.gc-photo'),frame=photo&&photo.querySelector('.gc-photo__frame'),"
        L"steam=photo&&photo.querySelector('.gc-steam');"
        L"if(!h||!root||!p||!k||!badge||!tag||!vf||!frame||!steam)f('missing banner geometry');"
        L"var hb=b(h),pb=b(p),kb=b(k),bb=b(badge),vb=b(vf),tb=tag.children;"
        L"var lm=kb.left-hb.left,rm=hb.right-pb.right;"
        L"if(Math.abs(lm-rm)>1.05)f('B1 margin');"
        L"for(var i=0;i<tb.length;i++){var tr=b(tb[i]);if(tr.top<bb.bottom-.05||Math.abs(tr.left-bb.left)>2.05)f('B2 slogan');}"
        L"var m=Math.min(vb.left-pb.left,vb.top-pb.top,pb.right-vb.right,pb.bottom-vb.bottom);"
        L"if(m<4-.05)f('B3 bracket margin');"
        L"if(document.body.className.indexOf('__never__')>=0)f('unreachable');"
        L"var active=p.classList.contains('is-active');"
        L"if(active){if(steam.parentNode!==frame||parseFloat(getComputedStyle(steam).opacity)<=.2)f('B4 steam hidden');"
        L"var fr=b(frame),scale=fr.width/800,c={left:fr.left+20*scale,top:fr.top+283*scale,right:fr.left+190*scale,bottom:fr.top+384*scale},st=b(steam);"
        L"if(st.right<=c.left||st.left>=c.right||st.bottom>c.top+2)f('B4 steam position');}"
        L"return true;})()";
    const bool ok = ExecuteScript(sponsor, script);
    std::printf("REAL_GEOMETRY width=%d state=%s=%s\n", width, state, ok ? "PASS" : "FAIL");
    return ok;
}

struct GdiplusGuard {
    ULONG_PTR token = 0;
    bool ok = false;
    GdiplusGuard() {
        gp::GdiplusStartupInput input;
        ok = gp::GdiplusStartup(&token, &input, nullptr) == gp::Ok;
    }
    ~GdiplusGuard() {
        if (ok) gp::GdiplusShutdown(token);
    }
};

HWND CreateHiddenOffscreenParent(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = ParentProc;
    wc.lpszClassName = kParentClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    // WebView2 requires a shown owner for this runtime configuration. SW_SHOWNA keeps it
    // off-screen and does not activate it; there is still no cursor movement or focus call.
    HWND parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kParentClass, L"",
                                  WS_POPUP, -1000, -1000, 976, kStripHeight, nullptr, nullptr,
                                  instance, nullptr);
    if (parent != nullptr) ShowWindow(parent, SW_SHOWNA);
    return parent;
}

int RunForcedFirstNavigationCase(HINSTANCE instance) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co)) return 1;
    GdiplusGuard gdiplus;
    if (!gdiplus.ok) {
        CoUninitialize();
        return 1;
    }
    HWND parent = CreateHiddenOffscreenParent(instance);
    if (parent == nullptr) {
        CoUninitialize();
        return 1;
    }
    ReadyState ready;
    const RECT initial{0, 0, 848, kStripHeight};
    cd::WebSponsorForceFirstNavigationFailureForTest();
    cd::WebSponsor* sponsor = cd::WebSponsorCreate(parent, initial, Ready, &ready);
    const bool pass = sponsor != nullptr && PumpUntil([&]() { return ready.called; }, 20000) &&
                      !ready.ok;
    std::printf("FIRST_NAV_FAILURE_FALLBACK=%s\n", pass ? "PASS" : "FAIL");
    if (sponsor != nullptr) cd::WebSponsorDestroy(sponsor);
    DestroyWindow(parent);
    CoUninitialize();
    return pass ? 0 : 1;
}

bool RunForcedFirstNavigationChild() {
    wchar_t exe[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (length == 0 || length >= std::size(exe)) return false;
    std::wstring command = L"\"";
    command.append(exe, length);
    command += L"\" --force-first";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return false;
    const DWORD wait = WaitForSingleObject(process.hProcess, 25000);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && code == 0;
}

}  // namespace

int main(int argc, char** argv) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (argc > 1 && std::string(argv[1]) == "--force-first")
        return RunForcedFirstNavigationCase(instance);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co)) {
        std::printf("HARNESS FAIL CoInitializeEx hr=0x%08lx\n",
                    static_cast<unsigned long>(co));
        return 1;
    }
    GdiplusGuard gdiplus;
    if (!gdiplus.ok) {
        std::printf("HARNESS FAIL GdiplusStartup\n");
        CoUninitialize();
        return 1;
    }

    HWND parent = CreateHiddenOffscreenParent(instance);
    if (parent == nullptr) {
        std::printf("HARNESS FAIL CreateWindow gle=%lu\n", GetLastError());
        CoUninitialize();
        return 1;
    }
    std::printf("HARNESS_PARENT visible=%d active=%d\n",
                IsWindowVisible(parent) ? 1 : 0,
                GetForegroundWindow() == parent ? 1 : 0);
    RECT parentRect{};
    GetWindowRect(parent, &parentRect);
    std::printf("HARNESS_PARENT_RECT left=%ld top=%ld right=%ld bottom=%ld\n",
                parentRect.left, parentRect.top, parentRect.right, parentRect.bottom);
    std::fflush(stdout);

    const std::streamoff logStart = LogSize();
    const RECT initial{0, 0, 848, kStripHeight};

    ReadyState ready;
    cd::WebSponsor* sponsor = cd::WebSponsorCreate(parent, initial, Ready, &ready);
    if (sponsor == nullptr) {
        std::printf("WEBVIEW_CREATE returned_null\n");
        std::printf("WEBVIEW_READY ok=0\n");
        PrintSponsorLogLines(logStart);
        DestroyWindow(parent);
        CoUninitialize();
        return 1;
    }
    const bool hiddenBeforeReady = !IsWindowVisible(cd::WebSponsorWindow(sponsor));
    if (!PumpUntil([&]() { return ready.called; }, 20000)) {
        std::printf("WEBVIEW_READY timeout\n");
        PrintSponsorLogLines(logStart);
        cd::WebSponsorDestroy(sponsor);
        DestroyWindow(parent);
        CoUninitialize();
        return 1;
    }
    PrintSponsorLogLines(logStart);
    if (!ready.ok) {
        cd::WebSponsorDestroy(sponsor);
        DestroyWindow(parent);
        CoUninitialize();
        return 1;
    }
    const bool visibleAfterReady = IsWindowVisible(cd::WebSponsorWindow(sponsor)) != FALSE;
    const bool visibilityGatePass = hiddenBeforeReady && visibleAfterReady;
    std::printf("HOST_VISIBILITY_GATE=%s\n", visibilityGatePass ? "PASS" : "FAIL");

    const fs::path output = fs::path("build") / "capture-webview2-real";
    std::error_code error;
    fs::create_directories(output, error);
    bool passed = !error && visibilityGatePass;
    PumpFor(2000);
    passed = Capture(sponsor, 848, output / "rest-848.png") && passed;
    passed = Capture(sponsor, 976, output / "rest-976.png") && passed;

    const std::string liveJson = ReadBytes(fs::path("build") / "live-message-061.json");
    const std::wstring liveMessage = Utf8ToWide(liveJson);
    if (liveMessage.empty() || !cd::WebSponsorPostMessageForTest(sponsor, liveMessage.c_str())) {
        std::printf("LIVE_MESSAGE FAIL\n");
        passed = false;
    } else {
        PumpFor(1000);
        passed = Capture(sponsor, 848, output / "live-848.png") && passed;
        passed = AssertGeometry(sponsor, 848, "rest") && passed;
        passed = Capture(sponsor, 976, output / "live-976.png") && passed;
        passed = AssertGeometry(sponsor, 976, "rest") && passed;
        passed = CropPlate2x(output / "live-848.png", 848, output / "live-crop2x-848.png") && passed;
        passed = CropPlate2x(output / "live-976.png", 976, output / "live-crop2x-976.png") && passed;
        const wchar_t hoverScript[] =
            L"(function(){var h=document.getElementById('h');"
            L"var p=h&&h.shadowRoot&&h.shadowRoot.querySelector('.goat-lockup');"
            L"if(!p)return false;p.classList.add('is-active');"
            L"p.dispatchEvent(new PointerEvent('pointerenter',{bubbles:true}));return true;})()";
        if (ExecuteScript(sponsor, hoverScript)) {
            PumpFor(700);
            passed = Capture(sponsor, 848, output / "hover-848.png") && passed;
            passed = AssertGeometry(sponsor, 848, "hover") && passed;
            passed = Capture(sponsor, 976, output / "hover-976.png") && passed;
            passed = AssertGeometry(sponsor, 976, "hover") && passed;
            passed = CropPlate2x(output / "hover-848.png", 848, output / "hover-crop2x-848.png") && passed;
            passed = CropPlate2x(output / "hover-976.png", 976, output / "hover-crop2x-976.png") && passed;
        } else {
            std::printf("HOVER skipped\n");
        }
    }

    cd::WebSponsorDestroy(sponsor);
    DestroyWindow(parent);
    const bool firstFailureFallback = RunForcedFirstNavigationChild();
    std::printf("FIRST_NAV_FAILURE_FALLBACK=%s\n", firstFailureFallback ? "PASS" : "FAIL");
    passed = firstFailureFallback && passed;
    CoUninitialize();
    std::printf("WEBVIEW_HARNESS FAIL=%d\n", passed ? 0 : 1);
    return passed ? 0 : 1;
}
