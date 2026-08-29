// Game Optimizer - the sponsor strip, rendered by an embedded WebView2.
// See webview_host.h for why this exists and the four rules it has to keep.
//
// ============================================================================
// THE COM DECLARATIONS BELOW ARE TRANSCRIBED, NOT REMEMBERED
// ============================================================================
// There is no WebView2.h on this machine. The interfaces are declared here by hand, and the
// two things that make that safe rather than a guess are the IID and the METHOD ORDER. Both
// were read out of Microsoft's own generated bindings:
//
//   D:\cargo\registry\src\index.crates.io-1949cf8c6b5b557f\webview2-com-sys-0.38.2\src\bindings.rs
//
// A wrong IID fails a QueryInterface, which is survivable. A method declared out of order is
// a call through the WRONG VTABLE SLOT: it compiles, it links, and it corrupts the stack at
// run time. So every interface below lists EVERY method up to the last one this file calls,
// in the order the bindings list them, including the ones that are never called - they are
// there to hold their slots. Parameter types for uncalled methods are deliberately loose
// (void*), because a slot's position is what matters, not its signature.
#include "webview_host.h"

#include <objbase.h>
#include <shellapi.h>
#include <new>
#include <string>
#include <string.h>

#include "sponsor.h"
#include "sponsor_html.h"
#include "util.h"

namespace cd {
namespace {

const wchar_t* const kHostClass = L"cd_SponsorWeb";

// The loader's filename, in ONE place.
//
// It is a named constant so the fallback can actually be TESTED rather than reasoned about:
// point this at a name that does not exist, rebuild, and every one of the three probes below
// misses, which is precisely the machine-without-WebView2 case. Reasoning about that path is
// not proof - it protects every user whose machine differs from the build machine, and it is
// the path that must never take the app down with it.
const wchar_t* const kLoaderDll = L"WebView2Loader.dll";

// ===========================================================================
// IIDs - transcribed from bindings.rs, line numbers as of webview2-com-sys 0.38.2.
// ===========================================================================
// 0x4e8a3389_c9d8_4bd2_b6b5_124fee6cc14d
const GUID kIID_EnvCompleted =
    { 0x4e8a3389, 0xc9d8, 0x4bd2, { 0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d } };
// 0x6c4819f3_c9b7_4260_8127_c9f5bde7f68c
const GUID kIID_CtrlCompleted =
    { 0x6c4819f3, 0xc9b7, 0x4260, { 0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c } };
// 0x9adbe429_f36d_432b_9ddc_f8881fbd76e3
const GUID kIID_NavStartingHandler =
    { 0x9adbe429, 0xf36d, 0x432b, { 0x9d, 0xdc, 0xf8, 0x88, 0x1f, 0xbd, 0x76, 0xe3 } };
// 0xd4c185fe_c81c_4989_97af_2d3fa7ab5651
const GUID kIID_NewWindowHandler =
    { 0xd4c185fe, 0xc81c, 0x4989, { 0x97, 0xaf, 0x2d, 0x3f, 0xa7, 0xab, 0x56, 0x51 } };
// 0xc979903e_d4ca_4228_92eb_47ee3fa96eab
const GUID kIID_Controller2 =
    { 0xc979903e, 0xd4ca, 0x4228, { 0x92, 0xeb, 0x47, 0xee, 0x3f, 0xa9, 0x6e, 0xab } };

// ===========================================================================
// Interfaces.
// ===========================================================================

// bindings.rs: pub struct COREWEBVIEW2_COLOR { A, R, G, B } - alpha FIRST.
struct WvColor {
    BYTE A;
    BYTE R;
    BYTE G;
    BYTE B;
};

struct IWvSettings : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_IsScriptEnabled(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsScriptEnabled(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsWebMessageEnabled(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsWebMessageEnabled(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AreDefaultScriptDialogsEnabled(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AreDefaultScriptDialogsEnabled(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsStatusBarEnabled(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsStatusBarEnabled(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AreDevToolsEnabled(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AreDevToolsEnabled(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AreDefaultContextMenusEnabled(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AreDefaultContextMenusEnabled(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AreHostObjectsAllowed(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AreHostObjectsAllowed(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsZoomControlEnabled(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsZoomControlEnabled(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsBuiltInErrorPageEnabled(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBuiltInErrorPageEnabled(BOOL) = 0;
};

// ICoreWebView2NavigationStartingEventArgs.
struct IWvNavStartingArgs : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsUserInitiated(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRedirected(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RequestHeaders(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Cancel(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Cancel(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NavigationId(UINT64*) = 0;
};

// ICoreWebView2NewWindowRequestedEventArgs.
struct IWvNewWindowArgs : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_NewWindow(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NewWindow(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Handled(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Handled(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsUserInitiated(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeferral(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_WindowFeatures(void**) = 0;
};

// ICoreWebView2. Every slot up to add_NewWindowRequested is listed; the ones this file never
// calls are here only to hold their positions.
struct IWvWebView : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Settings(IWvSettings**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Source(LPWSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Navigate(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE NavigateToString(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NavigationStarting(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_NavigationStarting(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ContentLoading(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ContentLoading(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_SourceChanged(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_SourceChanged(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_HistoryChanged(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_HistoryChanged(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NavigationCompleted(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_NavigationCompleted(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameNavigationStarting(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameNavigationStarting(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameNavigationCompleted(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameNavigationCompleted(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ScriptDialogOpening(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ScriptDialogOpening(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PermissionRequested(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_PermissionRequested(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ProcessFailed(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ProcessFailed(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddScriptToExecuteOnDocumentCreated(LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveScriptToExecuteOnDocumentCreated(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteScript(LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CapturePreview(INT32, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reload() = 0;
    virtual HRESULT STDMETHODCALLTYPE PostWebMessageAsJson(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE PostWebMessageAsString(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_WebMessageReceived(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_WebMessageReceived(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE CallDevToolsProtocolMethod(LPCWSTR, LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BrowserProcessId(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CanGoBack(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CanGoForward(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GoBack() = 0;
    virtual HRESULT STDMETHODCALLTYPE GoForward() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDevToolsProtocolEventReceiver(LPCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NewWindowRequested(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_NewWindowRequested(INT64) = 0;
};

// ICoreWebView2Controller.
struct IWvController : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_IsVisible(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsVisible(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Bounds(RECT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Bounds(RECT) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ZoomFactor(double*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ZoomFactor(double) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ZoomFactorChanged(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ZoomFactorChanged(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBoundsAndZoomFactor(RECT, double) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveFocus(INT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_MoveFocusRequested(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_MoveFocusRequested(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_GotFocus(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_GotFocus(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_LostFocus(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_LostFocus(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_AcceleratorKeyPressed(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_AcceleratorKeyPressed(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ParentWindow(HWND*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ParentWindow(HWND) = 0;
    virtual HRESULT STDMETHODCALLTYPE NotifyParentWindowPositionChanged() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CoreWebView2(IWvWebView**) = 0;
};

// ICoreWebView2Controller2 - derives from ICoreWebView2Controller, so its two methods sit
// AFTER the whole base vtable. Declaring the inheritance is what puts them there.
struct IWvController2 : public IWvController {
    virtual HRESULT STDMETHODCALLTYPE get_DefaultBackgroundColor(WvColor*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DefaultBackgroundColor(WvColor) = 0;
};

// ICoreWebView2Environment.
struct IWvEnvironment : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2Controller(HWND, IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateWebResourceResponse(void*, INT32, LPCWSTR, LPCWSTR,
                                                                void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BrowserVersionString(LPWSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NewBrowserVersionAvailable(IUnknown*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_NewBrowserVersionAvailable(INT64) = 0;
};

typedef HRESULT(STDAPICALLTYPE* PFN_CreateEnv)(PCWSTR browserFolder, PCWSTR userDataFolder,
                                               void* options, IUnknown* handler);

// ===========================================================================
// The loader DLL.
//
// LoadLibraryW, never a static import: an exe that names WebView2Loader.dll in its import
// table will not START on a machine that does not have it, which is exactly the failure this
// whole design exists to avoid. The exe's own directory first, so the copy tools\build.bat
// puts next to the binary wins; then the installed runtime folder; then the default search
// path, which finds a machine-wide or side-by-side copy if one exists.
//
// The handle is deliberately NOT freed. Chromium spins up background threads inside this DLL
// and unloading it while they run is a crash; the same reason the WebView2 samples never call
// FreeLibrary either. It is one module, loaded at most once, only on a Settings open.
// ===========================================================================
PFN_CreateEnv ResolveCreateEnv() {
    static PFN_CreateEnv fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;

    HMODULE mod = nullptr;

    const std::wstring exe = GetExePath();
    const size_t slash = exe.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        const std::wstring beside = exe.substr(0, slash + 1) + kLoaderDll;
        mod = ::LoadLibraryExW(beside.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (mod == nullptr) {
            LogLine(L"[sponsor] no loader beside the exe (%s), gle=%lu", beside.c_str(),
                    ::GetLastError());
        }
    }
    if (mod == nullptr) {
        // Some deployments drop the loader into the installed runtime's own folder. Probing
        // it costs nothing and is the documented second place to look.
        const std::wstring runtimeCopy =
            std::wstring(L"C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application\\") +
            kLoaderDll;
        mod = ::LoadLibraryExW(runtimeCopy.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (mod == nullptr) mod = ::LoadLibraryW(kLoaderDll);
    if (mod == nullptr) {
        LogLine(L"[sponsor] WebView2Loader.dll not found anywhere - using the GDI strip");
        return nullptr;
    }

    fn = reinterpret_cast<PFN_CreateEnv>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "CreateCoreWebView2EnvironmentWithOptions")));
    if (fn == nullptr) {
        LogLine(L"[sponsor] the loader has no CreateCoreWebView2EnvironmentWithOptions export");
    }
    return fn;
}

// ===========================================================================
// The page.
// ===========================================================================
// The page is COMPLETE as generated - no placeholders, no run-time substitution.
//
// It used to carry {{URL_KOFI}} and friends, patched here from sponsor.h. That was one
// mechanism too many: the generator already knows the three destinations, and a placeholder
// the generator forgot to emit fails SILENTLY - the substitution simply matches nothing, the
// button renders perfectly and does nothing when clicked. Measured: that is exactly what had
// happened to the Ko-fi and GitHub buttons, whose {{URL_*}} markers were not in the page at
// all while this function dutifully "substituted" them.
//
// So the URLs are baked in at generation time, and tools\gen-sponsor-html.py FAILS if they
// disagree with sponsor.h's constants - which is what OpenIfKnown below checks every
// navigation against. Two places still hold the URLs, but they are now verified equal by a
// build-time step rather than by hope.
std::wstring BuildPage() {
    std::wstring html;
    AppendSponsorHtml(html);
    return html;
}

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    const size_t n = ::wcslen(prefix);
    if (s.size() < n) return false;
    return _wcsnicmp(s.c_str(), prefix, n) == 0;
}

// The browser normalises "https://dagoat.io" to "https://dagoat.io/" before the navigation
// event fires, so an exact compare against the constant in sponsor.h would MISS the goat
// lockup - the one link most likely to be clicked. One optional trailing slash is the whole
// tolerance; nothing else is forgiven.
bool SameTarget(std::wstring a, std::wstring b) {
    if (!a.empty() && a[a.size() - 1] == L'/') a.erase(a.size() - 1);
    if (!b.empty() && b[b.size() - 1] == L'/') b.erase(b.size() - 1);
    return a.size() == b.size() &&
           ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// A click is only ever allowed to open one of the three known destinations, and what is
// opened is the CONSTANT from sponsor.h, never the string the page supplied. Anything else -
// a redirect, an injected link, a mistake in the page - is dropped. Opening a WRONG url is
// worse than opening none: it sends the user somewhere the author did not intend.
bool OpenIfKnown(HWND owner, const std::wstring& uri) {
    const wchar_t* const known[3] = { sponsor_url::kKofi, sponsor_url::kGitHub,
                                      sponsor_url::kGoatProject };
    for (int i = 0; i < 3; ++i) {
        if (SameTarget(uri, known[i])) {
            LogLine(L"[sponsor] opening %s in the default browser", known[i]);
            ::ShellExecuteW(owner, L"open", known[i], nullptr, nullptr, SW_SHOWNORMAL);
            return true;
        }
    }
    LogLine(L"[sponsor] refused an unexpected navigation target");
    return false;
}

}  // namespace

// ===========================================================================
// The object.
// ===========================================================================
struct WebSponsor {
    LONG ref = 1;            // one reference for the owner; handlers add their own
    bool closed = false;     // WebSponsorDestroy has run; pending callbacks must do nothing
    bool reported = false;   // `ready` is called exactly once

    HWND parent = nullptr;
    HWND host = nullptr;
    RECT rc = { 0, 0, 0, 0 };

    IWvController* controller = nullptr;
    IWvWebView* webview = nullptr;

    WebSponsorReadyFn ready = nullptr;
    void* user = nullptr;

    void AddRef() { ::InterlockedIncrement(&ref); }
    void Release() {
        if (::InterlockedDecrement(&ref) == 0) delete this;
    }

    void Report(bool ok) {
        if (reported) return;
        reported = true;
        if (ready != nullptr) ready(user, ok);
    }

    // Everything that touches the runtime, in one place, so the failure path and the destroy
    // path cannot drift apart.
    void Teardown() {
        if (webview != nullptr) {
            webview->Release();
            webview = nullptr;
        }
        if (controller != nullptr) {
            controller->Close();
            controller->Release();
            controller = nullptr;
        }
        if (host != nullptr) {
            ::DestroyWindow(host);
            host = nullptr;
        }
    }
};

namespace {

// ===========================================================================
// Handlers. Each is a COM object this file owns; each holds a reference on the WebSponsor so
// a Settings window that closes mid-creation cannot leave a callback pointing at freed
// memory. `closed` is what they check, not the pointer.
// ===========================================================================
// ---------------------------------------------------------------------------
// THE DESTRUCTOR HERE IS NOT VIRTUAL, AND THAT IS DELIBERATE.
//
// A virtual destructor declared in this base would take a vtable slot immediately after
// AddRef and Release, pushing each derived class's Invoke from slot 3 to slot 4. WebView2
// calls slot 3. That mistake compiles, links, and then calls the wrong function pointer at
// run time. Release() deletes through TSelf*, so the correct derived destructor still runs
// and nothing leaks - the virtual dispatch is simply not needed here.
//
// Members of a dependent base are referred to as this->owner_ / this->Qi(...) throughout,
// because /permissive- turns on real two-phase lookup and the unqualified names would not
// resolve.
// ---------------------------------------------------------------------------
template <typename TSelf>
class HandlerBase : public IUnknown {
public:
    explicit HandlerBase(WebSponsor* o) : owner_(o) { owner_->AddRef(); }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&ref_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = ::InterlockedDecrement(&ref_);
        if (n == 0) delete static_cast<TSelf*>(this);
        return static_cast<ULONG>(n);
    }

protected:
    ~HandlerBase() { owner_->Release(); }

    HRESULT Qi(REFIID riid, const GUID& own, void** out) {
        if (out == nullptr) return E_POINTER;
        *out = nullptr;
        if (::IsEqualGUID(riid, __uuidof(IUnknown)) || ::IsEqualGUID(riid, own)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    WebSponsor* owner_;

private:
    LONG ref_ = 1;
};

class NavStartingHandler : public HandlerBase<NavStartingHandler> {
public:
    explicit NavStartingHandler(WebSponsor* o) : HandlerBase(o) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        return this->Qi(riid, kIID_NavStartingHandler, out);
    }
    // Invoke(sender, args). Cancel EVERY http(s) navigation the page tries to make - nothing
    // may load inside this view - and hand a recognised URL to the user's real browser. The
    // initial NavigateToString is not http, so it is left alone.
    virtual HRESULT STDMETHODCALLTYPE Invoke(IWvWebView* sender, IWvNavStartingArgs* args) {
        (void)sender;
        if (args == nullptr) return S_OK;
        LPWSTR uri = nullptr;
        if (SUCCEEDED(args->get_Uri(&uri)) && uri != nullptr) {
            const std::wstring u(uri);
            ::CoTaskMemFree(uri);
            if (StartsWithI(u, L"http://") || StartsWithI(u, L"https://")) {
                args->put_Cancel(TRUE);
                if (!this->owner_->closed) OpenIfKnown(this->owner_->host, u);
            }
        }
        return S_OK;
    }
};

class NewWindowHandler : public HandlerBase<NewWindowHandler> {
public:
    explicit NewWindowHandler(WebSponsor* o) : HandlerBase(o) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        return this->Qi(riid, kIID_NewWindowHandler, out);
    }
    // target="_blank" arrives here instead of NavigationStarting, which is why both are
    // handled. Marking it handled without supplying a window is what stops WebView2 opening
    // one of its own.
    virtual HRESULT STDMETHODCALLTYPE Invoke(IWvWebView* sender, IWvNewWindowArgs* args) {
        (void)sender;
        if (args == nullptr) return S_OK;
        LPWSTR uri = nullptr;
        if (SUCCEEDED(args->get_Uri(&uri)) && uri != nullptr) {
            const std::wstring u(uri);
            ::CoTaskMemFree(uri);
            if (!this->owner_->closed) OpenIfKnown(this->owner_->host, u);
        }
        args->put_Handled(TRUE);
        return S_OK;
    }
};

class ControllerHandler : public HandlerBase<ControllerHandler> {
public:
    explicit ControllerHandler(WebSponsor* o) : HandlerBase(o) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        return this->Qi(riid, kIID_CtrlCompleted, out);
    }
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, IWvController* controller);
};

class EnvironmentHandler : public HandlerBase<EnvironmentHandler> {
public:
    explicit EnvironmentHandler(WebSponsor* o) : HandlerBase(o) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        return this->Qi(riid, kIID_EnvCompleted, out);
    }
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, IWvEnvironment* env) {
        WebSponsor* const o = this->owner_;
        if (o->closed) return S_OK;
        if (FAILED(hr) || env == nullptr) {
            LogLine(L"[sponsor] WebView2 environment creation failed, hr=0x%08lx - "
                    L"falling back to the GDI strip", static_cast<unsigned long>(hr));
            o->Teardown();
            o->Report(false);
            return S_OK;
        }
        ControllerHandler* h = new (std::nothrow) ControllerHandler(o);
        if (h == nullptr) {
            o->Teardown();
            o->Report(false);
            return S_OK;
        }
        const HRESULT chr = env->CreateCoreWebView2Controller(o->host, h);
        h->Release();
        if (FAILED(chr)) {
            LogLine(L"[sponsor] CreateCoreWebView2Controller failed, hr=0x%08lx",
                    static_cast<unsigned long>(chr));
            o->Teardown();
            o->Report(false);
        }
        return S_OK;
    }
};

HRESULT STDMETHODCALLTYPE ControllerHandler::Invoke(HRESULT hr, IWvController* controller) {
    // The base member is reached through this-> once, here; /permissive- two-phase lookup
    // will not find an unqualified name that lives in a dependent base.
    WebSponsor* const owner = this->owner_;
    if (owner->closed) {
        // The Settings window went away while the controller was being built. Close the one
        // we were handed rather than leaking a browser nobody can see.
        if (SUCCEEDED(hr) && controller != nullptr) controller->Close();
        return S_OK;
    }
    if (FAILED(hr) || controller == nullptr) {
        LogLine(L"[sponsor] WebView2 controller creation failed, hr=0x%08lx - "
                L"falling back to the GDI strip", static_cast<unsigned long>(hr));
        owner->Teardown();
        owner->Report(false);
        return S_OK;
    }

    controller->AddRef();
    owner->controller = controller;

    // Transparent, so the dark card behind the strip shows through and the page needs no
    // background of its own. Controller2 is the interface that carries it; if it is not
    // available the page would sit on white, which is worse than the GDI strip, so the whole
    // attempt is abandoned rather than shipped looking wrong.
    IWvController2* c2 = nullptr;
    if (SUCCEEDED(controller->QueryInterface(kIID_Controller2, reinterpret_cast<void**>(&c2))) &&
        c2 != nullptr) {
        WvColor clear;
        clear.A = 0;
        clear.R = 0;
        clear.G = 0;
        clear.B = 0;
        const HRESULT bhr = c2->put_DefaultBackgroundColor(clear);
        c2->Release();
        if (FAILED(bhr)) {
            LogLine(L"[sponsor] put_DefaultBackgroundColor failed, hr=0x%08lx - GDI strip",
                    static_cast<unsigned long>(bhr));
            owner->Teardown();
            owner->Report(false);
            return S_OK;
        }
    } else {
        LogLine(L"[sponsor] ICoreWebView2Controller2 unavailable, so the strip cannot be "
                L"transparent - falling back to the GDI strip");
        owner->Teardown();
        owner->Report(false);
        return S_OK;
    }

    IWvWebView* web = nullptr;
    if (FAILED(controller->get_CoreWebView2(&web)) || web == nullptr) {
        owner->Teardown();
        owner->Report(false);
        return S_OK;
    }
    owner->webview = web;

    IWvSettings* s = nullptr;
    if (SUCCEEDED(web->get_Settings(&s)) && s != nullptr) {
        s->put_AreDevToolsEnabled(FALSE);
        s->put_IsStatusBarEnabled(FALSE);
        s->put_AreDefaultContextMenusEnabled(FALSE);
        s->put_IsZoomControlEnabled(FALSE);
        s->Release();
    }

    {
        INT64 token = 0;
        NewWindowHandler* nw = new (std::nothrow) NewWindowHandler(owner);
        if (nw != nullptr) {
            web->add_NewWindowRequested(static_cast<IUnknown*>(nw), &token);
            nw->Release();
        }
    }
    {
        INT64 token = 0;
        NavStartingHandler* ns = new (std::nothrow) NavStartingHandler(owner);
        if (ns != nullptr) {
            web->add_NavigationStarting(static_cast<IUnknown*>(ns), &token);
            ns->Release();
        }
    }

    RECT client;
    ::SetRect(&client, 0, 0, owner->rc.right - owner->rc.left,
              owner->rc.bottom - owner->rc.top);
    controller->put_Bounds(client);

    const std::wstring page = BuildPage();
    const HRESULT nhr = web->NavigateToString(page.c_str());
    if (FAILED(nhr)) {
        LogLine(L"[sponsor] NavigateToString failed, hr=0x%08lx - GDI strip",
                static_cast<unsigned long>(nhr));
        owner->Teardown();
        owner->Report(false);
        return S_OK;
    }

    controller->put_IsVisible(TRUE);
    ::ShowWindow(owner->host, SW_SHOWNA);
    owner->Report(true);
    return S_OK;
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // The webview paints the whole client area, so erasing it first would only flash.
    if (msg == WM_ERASEBKGND) return 1;
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterHostClass() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc;
    ::ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kHostClass;
    ::RegisterClassExW(&wc);
}

// %LOCALAPPDATA%\GameOptimizer\webview2. It sits beside config.ini rather than in a temp
// folder so a user who wants the app gone can delete one directory and be done.
std::wstring UserDataFolder() {
    const std::wstring dir = GetConfigDir();
    if (dir.empty()) return std::wstring();
    std::wstring full = dir + L"\\webview2";
    ::CreateDirectoryW(full.c_str(), nullptr);
    return full;
}

}  // namespace

// ===========================================================================
// Public API.
// ===========================================================================
SIZE WebSponsorNaturalSize(int dpi) {
    // MulDiv rather than a float: it rounds to nearest and cannot overflow on the ranges dpi
    // takes, and it is what every other size in this project is scaled with.
    if (dpi <= 0) dpi = 96;
    SIZE s;
    s.cx = ::MulDiv(kSponsorCssWidth, dpi, 96);
    s.cy = ::MulDiv(kSponsorCssHeight, dpi, 96);
    return s;
}

WebSponsor* WebSponsorCreate(HWND parent, const RECT& rc, WebSponsorReadyFn ready, void* user) {
    if (parent == nullptr) return nullptr;

    PFN_CreateEnv create = ResolveCreateEnv();
    if (create == nullptr) return nullptr;

    const std::wstring udf = UserDataFolder();
    if (udf.empty()) {
        LogLine(L"[sponsor] no writable user-data folder - using the GDI strip");
        return nullptr;
    }

    RegisterHostClass();

    WebSponsor* w = new (std::nothrow) WebSponsor();
    if (w == nullptr) return nullptr;
    w->parent = parent;
    w->rc = rc;
    w->ready = ready;
    w->user = user;

    // Created hidden. It is shown only once the page is actually rendering, so a failed
    // creation never leaves a hole where the strip should be.
    w->host = ::CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
                                rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                                parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (w->host == nullptr) {
        LogLine(L"[sponsor] the webview host window could not be created, gle=%lu",
                ::GetLastError());
        w->Release();
        return nullptr;
    }

    EnvironmentHandler* h = new (std::nothrow) EnvironmentHandler(w);
    if (h == nullptr) {
        w->Teardown();
        w->Release();
        return nullptr;
    }
    const HRESULT hr = create(nullptr, udf.c_str(), nullptr, static_cast<IUnknown*>(h));
    h->Release();
    if (FAILED(hr)) {
        LogLine(L"[sponsor] CreateCoreWebView2EnvironmentWithOptions failed, hr=0x%08lx - "
                L"using the GDI strip", static_cast<unsigned long>(hr));
        w->Teardown();
        w->Release();
        return nullptr;
    }
    return w;
}

void WebSponsorMove(WebSponsor* w, const RECT& rc) {
    if (w == nullptr || w->closed) return;
    w->rc = rc;
    if (w->host != nullptr) {
        ::SetWindowPos(w->host, nullptr, rc.left, rc.top, rc.right - rc.left,
                       rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (w->controller != nullptr) {
        RECT client;
        ::SetRect(&client, 0, 0, rc.right - rc.left, rc.bottom - rc.top);
        w->controller->put_Bounds(client);
        w->controller->NotifyParentWindowPositionChanged();
    }
}

void WebSponsorDestroy(WebSponsor* w) {
    if (w == nullptr) return;
    w->closed = true;
    w->ready = nullptr;      // no verdict after the owner has gone
    w->Teardown();
    w->Release();
}

HWND WebSponsorWindow(const WebSponsor* w) {
    return w == nullptr ? nullptr : w->host;
}

}  // namespace cd
