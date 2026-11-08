#define NOMINMAX
#include "banner_host.h"

#include "banner_key.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace cd {
namespace {

constexpr wchar_t kHostClass[] = L"GameOptimizerBannerHost";
constexpr UINT kClientChanged = WM_APP + 0x4e;
constexpr std::size_t kMaxPageWebpBytes = 300000;

struct Subscriber {
    int token = 0;
    HWND target = nullptr;
    UINT message = 0;
    bool visible = false;
};

struct HostState {
    HWND window = nullptr;
    std::unique_ptr<ob::Client> client;
    std::vector<Subscriber> subscribers;
    int nextToken = 1;
#if defined(CD_TESTS) || defined(CD_SPONSOR_HARNESS)
    const ob::Model* testModel = nullptr;
    bool testExpired = false;
#endif
};

HostState& State() {
    static HostState state;
    return state;
}

std::int64_t UnixNowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ticks;
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    constexpr std::uint64_t kUnixEpoch100ns = 116444736000000000ULL;
    if (ticks.QuadPart <= kUnixEpoch100ns) return 0;
    return static_cast<std::int64_t>((ticks.QuadPart - kUnixEpoch100ns) / 10000ULL);
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    HostState* state = reinterpret_cast<HostState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<HostState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state != nullptr && message == kClientChanged) {
        for (const Subscriber& sub : state->subscribers) {
            if (sub.target != nullptr && sub.message != 0) {
                PostMessageW(sub.target, sub.message, 0, 0);
            }
        }
        return 0;
    }
    if (message == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void EnsureWindow(HostState& state) {
    if (state.window != nullptr) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &HostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kHostClass;
        registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) return;
    state.window = CreateWindowExW(0, kHostClass, L"", 0, 0, 0, 0, 0,
                                   HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), &state);
}

void EnsureClient(HostState& state) {
    EnsureWindow(state);
    if (state.client != nullptr || state.window == nullptr) return;

    std::wstring cacheDir = GetConfigDir();
    if (!cacheDir.empty()) {
        cacheDir += L"\\banner";
        CreateDirectoryW(cacheDir.c_str(), nullptr);
    }

    ob::Config config;
    config.host = L"goatcam.dagoat.io";
    config.https = true;
    config.port = 443;
    config.cacheDir = cacheDir;
    config.publicKey65 = &kBannerPublicKey;
    config.floor = kBannerRevisionFloor;
    config.kinds = { ob::Kind::Webp, ob::Kind::Jpg };
    config.userAgent = L"GameOptimizer/0.6.0";
    config.pathPrefix.clear();
    state.client = std::make_unique<ob::Client>(std::move(config), state.window, kClientChanged);
}

void NotifySubscribers(HostState& state) {
    if (state.window != nullptr) PostMessageW(state.window, kClientChanged, 0, 0);
}

std::wstring WithoutOneTrailingSlash(std::wstring value) {
    if (!value.empty() && value.back() == L'/') value.pop_back();
    return value;
}

bool SameTarget(const std::wstring& left, const std::wstring& right) {
    const std::wstring a = WithoutOneTrailingSlash(left);
    const std::wstring b = WithoutOneTrailingSlash(right);
    return a.size() == b.size() &&
           CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring BannerLink(const std::string& link) {
    if (link.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, link.data(),
                                      static_cast<int>(link.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, link.data(),
                            static_cast<int>(link.size()), &out[0], n) != n) {
        return std::wstring();
    }
    return out;
}

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size() + 8);
    for (std::size_t i = 0; i < value.size(); ++i) {
        const wchar_t ch = value[i];
        if (ch >= 0xd800 && ch <= 0xdbff) {
            if (i + 1 < value.size() && value[i + 1] >= 0xdc00 && value[i + 1] <= 0xdfff) {
                out.push_back(ch);
                out.push_back(value[++i]);
            } else {
                out += L"\\uFFFD";
            }
            continue;
        }
        if (ch >= 0xdc00 && ch <= 0xdfff) {
            out += L"\\uFFFD";
            continue;
        }
        switch (ch) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\b': out += L"\\b"; break;
            case L'\f': out += L"\\f"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            case 0x2028: out += L"\\u2028"; break;
            case 0x2029: out += L"\\u2029"; break;
            default:
                if (ch < 0x20) {
                    wchar_t escaped[7]{};
                    swprintf_s(escaped, L"\\u%04x", static_cast<unsigned int>(ch));
                    out += escaped;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

std::string Base64Encode(const std::string& bytes) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned int a = static_cast<unsigned char>(bytes[i]);
        const unsigned int b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned int c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        const unsigned int triple = (a << 16) | (b << 8) | c;
        out.push_back(alphabet[(triple >> 18) & 0x3f]);
        out.push_back(alphabet[(triple >> 12) & 0x3f]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(triple >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < bytes.size() ? alphabet[triple & 0x3f] : '=');
    }
    return out;
}

std::wstring JsonString(const std::wstring& value) {
    return L"\"" + JsonEscape(value) + L"\"";
}

std::wstring JsonStringUtf8(const std::string& value) {
    return JsonString(BannerLink(value));
}

std::wstring JsonNumber(double value) {
    if (!std::isfinite(value)) value = 0.0;
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << value;
    const std::string text = stream.str();
    return std::wstring(text.begin(), text.end());
}

std::wstring JsonBool(bool value) {
    return value ? L"true" : L"false";
}

void AddComma(std::wstring* out, bool* first) {
    if (!*first) out->push_back(L',');
    *first = false;
}

void AddStringMember(std::wstring* out, bool* first, const wchar_t* key,
                     const std::wstring& value) {
    AddComma(out, first);
    *out += JsonString(key);
    out->push_back(L':');
    *out += JsonString(value);
}

void AddUtf8Member(std::wstring* out, bool* first, const wchar_t* key,
                   const std::string& value) {
    AddComma(out, first);
    *out += JsonString(key);
    out->push_back(L':');
    *out += JsonStringUtf8(value);
}

void AddBoolMember(std::wstring* out, bool* first, const wchar_t* key, bool value) {
    AddComma(out, first);
    *out += JsonString(key);
    out->push_back(L':');
    *out += JsonBool(value);
}

void AddIntMember(std::wstring* out, bool* first, const wchar_t* key, int value) {
    AddComma(out, first);
    *out += JsonString(key);
    out->push_back(L':');
    *out += std::to_wstring(value);
}

void AddNumberArray(std::wstring* out, const double* values, std::size_t count) {
    out->push_back(L'[');
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) out->push_back(L',');
        *out += JsonNumber(values[i]);
    }
    out->push_back(L']');
}

}  // namespace

bool SponsorLinkAllowed(const std::wstring& uri, const ob::Model& current) {
    const wchar_t* const fixed[] = {
        sponsor_url::kKofi,
        sponsor_url::kGitHub,
        sponsor_url::kGoatCam,
        sponsor_url::kDaGoat,
    };
    for (const wchar_t* target : fixed) {
        if (SameTarget(uri, target)) return true;
    }
    if (!current.online) return false;
    const std::wstring verified = BannerLink(current.banner.link);
    return !verified.empty() && SameTarget(uri, verified);
}

int BannerHostSubscribe(HWND target, UINT message) {
    if (target == nullptr || message == 0) return 0;
    HostState& state = State();
    EnsureClient(state);
    const int token = state.nextToken++;
    state.subscribers.push_back(Subscriber{token, target, message, false});
    return token;
}

void BannerHostUnsubscribe(int token) {
    if (token == 0) return;
    HostState& state = State();
    const auto it = std::find_if(state.subscribers.begin(), state.subscribers.end(),
                                 [token](const Subscriber& sub) { return sub.token == token; });
    if (it == state.subscribers.end()) return;
    if (it->visible) {
        it->visible = false;
        const bool anyVisible = std::any_of(state.subscribers.begin(), state.subscribers.end(),
                                            [](const Subscriber& sub) { return sub.visible; });
        if (!anyVisible && state.client != nullptr) state.client->Hidden();
    }
    state.subscribers.erase(it);
}

void BannerHostVisible(int token, bool visible) {
    if (token == 0) return;
    HostState& state = State();
    EnsureClient(state);
    const auto it = std::find_if(state.subscribers.begin(), state.subscribers.end(),
                                 [token](const Subscriber& sub) { return sub.token == token; });
    if (it == state.subscribers.end() || it->visible == visible) return;
    bool anyBefore = false;
    for (const Subscriber& sub : state.subscribers) anyBefore = anyBefore || sub.visible;
    it->visible = visible;
    bool anyAfter = false;
    for (const Subscriber& sub : state.subscribers) anyAfter = anyAfter || sub.visible;
    if (!anyBefore && anyAfter && state.client != nullptr) state.client->Shown();
    if (anyBefore && !anyAfter && state.client != nullptr) state.client->Hidden();
}

void BannerHostPointer(bool over) {
    HostState& state = State();
    EnsureClient(state);
    if (state.client != nullptr) state.client->Pointer(over);
}

void BannerHostFocus(bool inside) {
    HostState& state = State();
    EnsureClient(state);
    if (state.client != nullptr) state.client->Focus(inside);
}

bool BannerHostClick(std::string* url) {
    HostState& state = State();
#if defined(CD_TESTS) || defined(CD_SPONSOR_HARNESS)
    if (state.testModel != nullptr) {
        const ob::Model current = BannerHostCurrent();
        if (!current.online || current.banner.link.empty()) return false;
        if (url != nullptr) *url = current.banner.link;
        return true;
    }
#endif
    EnsureClient(state);
    return state.client != nullptr && state.client->Click(url);
}

void BannerHostResumed() {
    HostState& state = State();
#if defined(CD_TESTS) || defined(CD_SPONSOR_HARNESS)
    if (state.testModel != nullptr && state.testModel->online &&
        state.testModel->banner.expiresMs <= UnixNowMs()) {
        state.testExpired = true;
        NotifySubscribers(state);
    }
#endif
    EnsureClient(state);
    if (state.client != nullptr) state.client->Resumed();
}

ob::Model BannerHostCurrent() {
    HostState& state = State();
#if defined(CD_TESTS) || defined(CD_SPONSOR_HARNESS)
    if (state.testModel != nullptr) {
        ob::Model out = *state.testModel;
        if (state.testExpired) out.online = false;
        return out;
    }
#endif
    EnsureClient(state);
    return state.client != nullptr ? state.client->Current() : ob::Model();
}

std::wstring PageMessageJson(const ob::Model& model) {
    const bool online = model.online && !model.webp.empty() && model.webp.size() <= kMaxPageWebpBytes;
    std::wstring out = L"{\"goatcamBanner\":{";
    if (!online) {
        out += L"\"kind\":\"dagoat\"}}";
        return out;
    }

    out += L"\"kind\":\"online\",\"banner\":{";
    bool first = true;
    AddStringMember(&out, &first, L"headline", model.banner.headline);
    if (model.banner.hasHeadlineEm) AddStringMember(&out, &first, L"headlineEm", model.banner.headlineEm);
    AddStringMember(&out, &first, L"wordmark", model.banner.wordmark);
    AddComma(&out, &first);
    out += L"\"tagline\":[";
    for (std::size_t i = 0; i < model.banner.tagline.size(); ++i) {
        if (i != 0) out.push_back(L',');
        out += JsonString(model.banner.tagline[i]);
    }
    out.push_back(L']');
    if (model.banner.hasBadge) AddStringMember(&out, &first, L"badge", model.banner.badge);
    AddComma(&out, &first);
    out += L"\"effects\":{";
    bool effectsFirst = true;
    AddBoolMember(&out, &effectsFirst, L"viewfinder", model.banner.viewfinder);
    AddBoolMember(&out, &effectsFirst, L"steam", model.banner.steam);
    out += L'}';
    AddUtf8Member(&out, &first, L"link", model.banner.link);
    AddComma(&out, &first);
    out += L"\"labels\":{";
    bool labelsFirst = true;
    AddStringMember(&out, &labelsFirst, L"link", model.banner.labelLink);
    AddStringMember(&out, &labelsFirst, L"qr", model.banner.labelQr);
    out += L'}';
    AddComma(&out, &first);
    out += L"\"qr\":{";
    bool qrFirst = true;
    AddIntMember(&out, &qrFirst, L"size", 37);
    AddComma(&out, &qrFirst);
    out += L"\"rows\":[";
    for (std::size_t i = 0; i < model.banner.qrRows.size(); ++i) {
        if (i != 0) out.push_back(L',');
        out += JsonStringUtf8(model.banner.qrRows[i]);
    }
    out += L"]}";
    AddComma(&out, &first);
    out += L"\"photo\":{";
    bool photoFirst = true;
    AddIntMember(&out, &photoFirst, L"w", model.banner.w);
    AddIntMember(&out, &photoFirst, L"h", model.banner.h);
    AddComma(&out, &photoFirst);
    out += L"\"focus\":";
    AddNumberArray(&out, model.banner.focus, 2);
    AddComma(&out, &photoFirst);
    out += L"\"textBox\":";
    AddNumberArray(&out, model.banner.textBox, 4);
    AddComma(&out, &photoFirst);
    out += L"\"steamAt\":";
    AddNumberArray(&out, model.banner.steamAt, 2);
    out += L"}}";

    out += L",\"photo\":{\"kind\":\"webp\",\"dataUrl\":";
    out += JsonString(BannerLink("data:image/webp;base64," + Base64Encode(model.webp)));
    out += L"}}}";
    return out;
}

#if defined(CD_TESTS) || defined(CD_SPONSOR_HARNESS)
void BannerHostSetModelForTest(const ob::Model* model) {
    HostState& state = State();
    state.testModel = model;
    state.testExpired = false;
    EnsureWindow(state);
    NotifySubscribers(state);
}
#endif

}  // namespace cd
