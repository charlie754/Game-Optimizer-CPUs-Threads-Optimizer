#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "banner_host.h"
#include "banner_key.h"
#include "online_banner_client.h"
#include "sponsor.h"

namespace gp = Gdiplus;
namespace fs = std::filesystem;

namespace {

constexpr UINT kLiveChanged = WM_APP + 0x5d;
int g_fail = 0;

void Fail(const char* what) {
    ++g_fail;
    std::printf("FAIL %s\n", what);
}

std::string ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        &out[0], n, nullptr, nullptr);
    return out;
}

int EncoderClsid(const WCHAR* mime, CLSID* out) {
    UINT count = 0, bytes = 0;
    if (gp::GetImageEncodersSize(&count, &bytes) != gp::Ok || bytes == 0) return 0;
    std::vector<BYTE> storage(bytes);
    gp::ImageCodecInfo* info = reinterpret_cast<gp::ImageCodecInfo*>(storage.data());
    if (gp::GetImageEncoders(count, bytes, info) != gp::Ok) return 0;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) {
            *out = info[i].Clsid;
            return 1;
        }
    }
    return 0;
}

struct Capture {
    bool ok = false;
    std::vector<BYTE> pixels;
};

Capture CaptureWindow(HWND hwnd, int width, int height, const fs::path& path) {
    Capture result;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (bitmap == nullptr || dc == nullptr || bits == nullptr) {
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
        return result;
    }
    HGDIOBJ old = SelectObject(dc, bitmap);
    PatBlt(dc, 0, 0, width, height, BLACKNESS);
    SendMessageW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    result.pixels.assign(static_cast<BYTE*>(bits), static_cast<BYTE*>(bits) +
                         static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (size_t i = 3; i < result.pixels.size(); i += 4) result.pixels[i] = 255;

    gp::Bitmap image(width, height, width * 4, PixelFormat32bppPARGB,
                     result.pixels.data());
    CLSID png{};
    const bool saved = image.GetLastStatus() == gp::Ok && EncoderClsid(L"image/png", &png) != 0 &&
                       image.Save(path.wstring().c_str(), &png, nullptr) == gp::Ok;
    result.ok = saved;
    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(bitmap);
    return result;
}

bool PixelIsDark(const std::vector<BYTE>& pixels, int width, int x, int y) {
    if (x < 0 || y < 0 || x >= width) return false;
    const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                      static_cast<size_t>(x)) * 4;
    if (at + 2 >= pixels.size()) return false;
    const int b = pixels[at], g = pixels[at + 1], r = pixels[at + 2];
    return r + g + b < 300;
}

bool RectValid(const RECT& r) {
    return r.right > r.left && r.bottom > r.top;
}

bool RectInside(const RECT& inner, const RECT& outer) {
    return inner.left >= outer.left && inner.top >= outer.top &&
           inner.right <= outer.right && inner.bottom <= outer.bottom;
}

bool RectsOverlap(const RECT& a, const RECT& b) {
    RECT hit{};
    return IntersectRect(&hit, &a, &b) != FALSE;
}

bool CopyInkPixel(const BYTE* pixel) {
    const int b = pixel[0], g = pixel[1], r = pixel[2];
    return r >= 130 && g >= 120 && b >= 100 && r - g < 40 && g - b < 40;
}

RECT InkBounds(const Capture& capture, int width, const RECT& rect) {
    RECT out{width, static_cast<LONG>(capture.pixels.size() / 4 / width), 0, 0};
    if (!RectValid(rect)) return {};
    const int height = static_cast<int>(capture.pixels.size() / 4 / width);
    const int left = std::max(0L, rect.left), top = std::max(0L, rect.top);
    const int right = std::min(width, static_cast<int>(rect.right));
    const int bottom = std::min(height, static_cast<int>(rect.bottom));
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const BYTE* pixel = capture.pixels.data() +
                                (static_cast<size_t>(y) * width + x) * 4;
            if (!CopyInkPixel(pixel)) continue;
            out.left = std::min(out.left, static_cast<LONG>(x));
            out.top = std::min(out.top, static_cast<LONG>(y));
            out.right = std::max(out.right, static_cast<LONG>(x + 1));
            out.bottom = std::max(out.bottom, static_cast<LONG>(y + 1));
        }
    }
    return RectValid(out) ? out : RECT{};
}

bool CopyCentered(const Capture& capture, int width, int height,
                  const cd::SponsorHarnessLayout& layout, int* centre2) {
    const RECT ink = InkBounds(capture, width, layout.copy);
    if (!RectValid(ink)) return false;
    if (centre2 != nullptr) *centre2 = ink.top + ink.bottom;
    return std::abs((ink.top + ink.bottom) - height) <= 4;
}

bool IsMeteorOrBurst(const BYTE* pixel) {
    const int b = pixel[0], g = pixel[1], r = pixel[2];
    const bool meteor = std::abs(r - 0x79) <= 2 && std::abs(g - 0x5b) <= 2 &&
                        std::abs(b - 0x2e) <= 2;
    const bool burst = std::abs(r - 0xf0) <= 2 && std::abs(g - 0xc0) <= 2 &&
                      std::abs(b - 0x7a) <= 2;
    return meteor || burst;
}

bool BurstOutsideFallbackText(const Capture& capture, int width,
                              const cd::SponsorHarnessLayout& layout) {
    const RECT forbidden[2] = {layout.fallbackWordmark, layout.fallbackTag};
    const int height = static_cast<int>(capture.pixels.size() / 4 / width);
    for (const RECT& rect : forbidden) {
        if (!RectValid(rect)) return false;
        const int left = std::max(0L, rect.left), top = std::max(0L, rect.top);
        const int right = std::min(width, static_cast<int>(rect.right));
        const int bottom = std::min(height, static_cast<int>(rect.bottom));
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                const BYTE* pixel = capture.pixels.data() +
                                    (static_cast<size_t>(y) * width + x) * 4;
                if (IsMeteorOrBurst(pixel)) return false;
            }
        }
    }
    return true;
}

bool RegionNonUniform(const Capture& capture, int width, const RECT& rect) {
    if (!RectValid(rect)) return false;
    const int l = std::max(0L, rect.left), t = std::max(0L, rect.top);
    const int r = std::min(width, static_cast<int>(rect.right));
    const int b = std::min(static_cast<int>(capture.pixels.size() / 4 / width),
                           static_cast<int>(rect.bottom));
    if (r <= l || b <= t) return false;
    const size_t first = (static_cast<size_t>(t) * width + l) * 4;
    int changed = 0;
    const int total = (r - l) * (b - t);
    for (int y = t; y < b; ++y) {
        for (int x = l; x < r; ++x) {
            const size_t at = (static_cast<size_t>(y) * width + x) * 4;
            if (capture.pixels[at] != capture.pixels[first] ||
                capture.pixels[at + 1] != capture.pixels[first + 1] ||
                capture.pixels[at + 2] != capture.pixels[first + 2]) {
                ++changed;
            }
        }
    }
    return changed * 100 >= total;
}

double PlateNonBackground(const Capture& capture, int width, const RECT& plate) {
    if (!RectValid(plate)) return 0.0;
    const int h = static_cast<int>(capture.pixels.size() / 4 / width);
    int nonBackground = 0, total = 0;
    for (int y = std::max(0L, plate.top); y < std::min(h, static_cast<int>(plate.bottom)); ++y) {
        for (int x = std::max(0L, plate.left); x < std::min(width, static_cast<int>(plate.right)); ++x) {
            const size_t at = (static_cast<size_t>(y) * width + x) * 4;
            const int b = capture.pixels[at], g = capture.pixels[at + 1], r = capture.pixels[at + 2];
            if (std::abs(r - 11) > 12 || std::abs(g - 11) > 12 || std::abs(b - 13) > 12)
                ++nonBackground;
            ++total;
        }
    }
    return total == 0 ? 0.0 : static_cast<double>(nonBackground) / total;
}

bool VerifyQr(const Capture& capture, int width, const ob::Model& model,
              const cd::SponsorHarnessLayout& layout) {
    if (!capture.ok || model.banner.qrRows.size() != 37) return false;
    const int module = layout.module;
    const int qrLeft = layout.qrOrigin.x;
    const int qrTop = layout.qrOrigin.y;
    if (module < 2 || !RectInside(layout.tile, layout.plate) ||
        !RectInside(layout.tile, layout.viewfinder)) return false;
    for (int y = 0; y < 37; ++y) {
        if (model.banner.qrRows[static_cast<size_t>(y)].size() != 37) return false;
        for (int x = 0; x < 37; ++x) {
            const bool expected = model.banner.qrRows[static_cast<size_t>(y)][static_cast<size_t>(x)] == '1';
            const int sx = qrLeft + x * module + module / 2;
            const int sy = qrTop + y * module + module / 2;
            if (PixelIsDark(capture.pixels, width, sx, sy) != expected) return false;
        }
    }
    return true;
}

bool VerifyOnlineLayout(const cd::SponsorHarnessLayout& layout) {
    if (!RectInside(layout.tile, layout.plate) || !RectInside(layout.viewfinder, layout.plate) ||
        !RectInside(layout.stage, layout.plate) || !RectInside(layout.wordmark, layout.plate) ||
        !RectInside(layout.tagline[0], layout.plate) || !RectInside(layout.badge, layout.plate)) {
        return false;
    }
    if (RectsOverlap(layout.wordmark, layout.badge) ||
        RectsOverlap(layout.tagline[0], layout.badge) ||
        (RectValid(layout.tagline[1]) && RectsOverlap(layout.tagline[1], layout.badge)) ||
        (RectValid(layout.tagline[1]) && RectsOverlap(layout.wordmark, layout.tagline[1])) ||
        (RectValid(layout.tagline[0]) && RectsOverlap(layout.wordmark, layout.tagline[0]))) {
        return false;
    }
    return true;
}

bool VerifyOnlineLayout(const cd::SponsorHarnessLayout& layout, int dpi) {
    if (!VerifyOnlineLayout(layout)) return false;
    const int required = MulDiv(6, dpi, 96);
    return RectValid(layout.iphone) && layout.iphone.left - layout.text.right >= required &&
           layout.tile.left - layout.iphone.right >= required;
}

bool VerifyPlateRightEdge(const cd::SponsorHarnessLayout& layout, int width) {
    return layout.plate.right == width;
}

bool VerifyQrMargin(const cd::SponsorHarnessLayout& layout, int dpi) {
    const int required = MulDiv(4, dpi, 96);
    return layout.tile.top - layout.plate.top >= required &&
           layout.plate.bottom - layout.tile.bottom >= required &&
           layout.tile.left - layout.plate.left >= required &&
           layout.plate.right - layout.tile.right >= required;
}

bool VerifyTagEmBaseline(const cd::SponsorHarnessLayout& layout) {
    for (int i = 0; i < 2; ++i) {
        if (layout.taglineHasEm[i] && layout.tagBaseline[i] != layout.tagEmBaseline[i])
            return false;
    }
    return true;
}

bool AlphaOpaque(const Capture& capture) {
    for (size_t i = 3; i < capture.pixels.size(); i += 4) {
        if (capture.pixels[i] != 255) return false;
    }
    return !capture.pixels.empty();
}

double ReferenceDifference(const Capture& capture, int width, const fs::path& reference) {
    gp::Bitmap image(reference.wstring().c_str(), FALSE);
    if (image.GetLastStatus() != gp::Ok || image.GetWidth() == 0 || image.GetHeight() == 0)
        return 1.0;
    const int refW = static_cast<int>(image.GetWidth());
    const int refH = static_cast<int>(image.GetHeight());
    const int height = static_cast<int>(capture.pixels.size() / 4 / width);
    size_t different = 0, total = 0;
    for (int y = 0; y < height; ++y) {
        const int sy = std::min(refH - 1, y * refH / std::max(1, height));
        for (int x = 0; x < width; ++x) {
            const int sx = std::min(refW - 1, x * refW / std::max(1, width));
            gp::Color c;
            if (image.GetPixel(sx, sy, &c) != gp::Ok) return 1.0;
            const size_t at = (static_cast<size_t>(y) * width + x) * 4;
            if (std::abs(static_cast<int>(capture.pixels[at + 2]) - c.GetRed()) > 16 ||
                std::abs(static_cast<int>(capture.pixels[at + 1]) - c.GetGreen()) > 16 ||
                std::abs(static_cast<int>(capture.pixels[at]) - c.GetBlue()) > 16) {
                ++different;
            }
            ++total;
        }
    }
    return total == 0 ? 1.0 : static_cast<double>(different) / total;
}

ob::Model FixtureModel(const std::string& jpg) {
    ob::Model model;
    model.online = true;
    model.banner.headline = L"GOAT Cam \u2014 your iPhone, NOW your PC's webcam.";
    model.banner.headlineEm = L"NOW";
    model.banner.hasHeadlineEm = true;
    model.banner.wordmark = L"GOAT Cam";
    model.banner.tagline.push_back(L"NOW");
    model.banner.badge = L"LIVE PREVIEW";
    model.banner.hasBadge = true;
    model.banner.link = "https://goatcam.dagoat.io";
    model.banner.qrText = "https://goatcam.dagoat.io";
    model.banner.qrRows.assign(37, std::string(37, '0'));
    for (int y = 0; y < 37; ++y) {
        for (int x = 0; x < 37; ++x) {
            const bool finder = (x < 7 && y < 7) || (x >= 30 && y < 7) ||
                                (x < 7 && y >= 30);
            const bool edge = finder && (x == 0 || x == 6 || y == 0 || y == 6 ||
                                         (x >= 2 && x <= 4 && y >= 2 && y <= 4));
            model.banner.qrRows[static_cast<size_t>(y)][static_cast<size_t>(x)] =
                (edge || ((x * 13 + y * 7) % 11 == 0)) ? '1' : '0';
        }
    }
    model.banner.viewfinder = true;
    model.banner.steam = true;
    model.banner.w = 800;
    model.banner.h = 498;
    model.jpg = jpg;
    return model;
}

LRESULT CALLBACK LiveProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND MakeLiveNotify() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &LiveProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GameOptimizerSponsorCaptureLive";
        registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    return CreateWindowExW(0, L"GameOptimizerSponsorCaptureLive", L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
}

bool FetchLive(const fs::path& cache, ob::Model* model) {
    HWND notify = MakeLiveNotify();
    if (notify == nullptr) return false;
    ob::Config config;
    config.host = L"goatcam.dagoat.io";
    config.port = 443;
    config.https = true;
    config.cacheDir = cache.wstring();
    config.publicKey65 = &kBannerPublicKey;
    config.floor = kBannerRevisionFloor;
    config.kinds = {ob::Kind::Webp, ob::Kind::Jpg};
    config.userAgent = L"GameOptimizer/0.6.0";
    CreateDirectoryW(cache.wstring().c_str(), nullptr);
    ob::Client client(std::move(config), notify, kLiveChanged);
    client.Shown();
    const ULONGLONG deadline = GetTickCount64() + 30000;
    for (;;) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        *model = client.Current();
        if (model->online) {
            std::printf("live revision=%lld link=%s headline=%s\n",
                        static_cast<long long>(model->banner.revision),
                        model->banner.link.c_str(), Utf8(model->banner.headline).c_str());
            DestroyWindow(notify);
            return true;
        }
        if (GetTickCount64() >= deadline) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
    }
    *model = client.Current();
    std::printf("live revision=none link=none headline=none\n");
    DestroyWindow(notify);
    return false;
}

std::wstring LegacyJsonEscape(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size() + 8);
    for (wchar_t ch : value) {
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

bool WriteMessageJson(const fs::path& path, const ob::Model& model, bool legacy) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    std::string json = Utf8(cd::PageMessageJson(model));
    if (json.empty()) return false;
    if (legacy && model.online && !model.webp.empty()) {
        std::wstring oldData = L"data:image/webp;base64,";
        oldData.append(model.webp.begin(), model.webp.end());
        const std::string oldValue = Utf8(LegacyJsonEscape(oldData));
        const std::string marker = "\"dataUrl\":\"";
        const size_t begin = json.find(marker);
        if (begin == std::string::npos) return false;
        const size_t valueBegin = begin + marker.size();
        const size_t valueEnd = json.find('"', valueBegin);
        if (valueEnd == std::string::npos) return false;
        json.replace(valueBegin, valueEnd - valueBegin, oldValue);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!out) return false;
    std::printf("message-json legacy=%s bytes=%zu file=%s\n", legacy ? "true" : "false",
                json.size(), path.string().c_str());
    return true;
}

bool PixelsDiffer(const Capture& a, const Capture& b) {
    if (a.pixels.size() != b.pixels.size()) return true;
    size_t different = 0;
    for (size_t i = 0; i + 3 < a.pixels.size(); i += 4) {
        if (a.pixels[i] != b.pixels[i] || a.pixels[i + 1] != b.pixels[i + 1] ||
            a.pixels[i + 2] != b.pixels[i + 2]) ++different;
    }
    return different > 100;
}

bool CaptureAll(const fs::path& output, const ob::Model& online) {
    std::error_code error;
    fs::create_directories(output, error);
    if (error) return false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    cd::SponsorRegister(instance);
    ob::Model offline;
    cd::BannerHostSetModelForTest(&offline);
    Capture firstDaGoat;
    Capture firstOnline;
    for (int dpi : {96, 144, 192}) {
        cd::SponsorSetDpiForTest(dpi);
        const SIZE size = cd::SponsorMeasure(dpi);
        if (size.cx <= 0 || size.cy <= 0) {
            Fail("SponsorMeasure returned an invalid size");
            continue;
        }
        for (int kind = 0; kind < 2; ++kind) {
            const bool isOnline = kind != 0;
            cd::BannerHostSetModelForTest(isOnline ? &online : &offline);
            cd::SponsorHarnessLayout restLayout{};
            bool haveRestLayout = false;
            for (int hover = 0; hover < 2; ++hover) {
                HWND hwnd = CreateWindowExW(0, cd::kSponsorClass, L"", WS_POPUP,
                                            0, 0, size.cx, size.cy, nullptr, nullptr,
                                            instance, nullptr);
                if (hwnd == nullptr) {
                    Fail("CreateWindowExW sponsor capture");
                    continue;
                }
                cd::SponsorSetHoverForTest(hwnd, hover != 0);
                cd::SponsorHarnessLayout layout{};
                if (!cd::SponsorGetLayoutForTest(hwnd, &layout)) {
                    Fail("layout seam");
                }
                const fs::path file = output / (std::string(isOnline ? "online-" : "dagoat-") +
                                                std::to_string(dpi) + (hover ? "-hover.png" : "-rest.png"));
                Capture capture = CaptureWindow(hwnd, size.cx, size.cy, file);
                std::printf("capture dpi=%d mode=%s state=%s file=%s bytes=%zu\n", dpi,
                            isOnline ? "online" : "dagoat", hover ? "hover" : "rest",
                            file.string().c_str(), capture.pixels.size());
                if (!capture.ok) Fail("PNG capture");
                const bool alphaOk = AlphaOpaque(capture);
                std::printf("check dpi=%d mode=%s state=%s alpha=%s\n", dpi,
                            isOnline ? "online" : "dagoat", hover ? "hover" : "rest",
                            alphaOk ? "PASS" : "FAIL");
                if (!alphaOk) Fail("A1 PNG alpha not opaque");
                const bool buttonsOk = RegionNonUniform(capture, size.cx, layout.buttons[0]) &&
                                       RegionNonUniform(capture, size.cx, layout.buttons[1]);
                std::printf("check dpi=%d mode=%s state=%s buttons=%s\n", dpi,
                            isOnline ? "online" : "dagoat", hover ? "hover" : "rest",
                            buttonsOk ? "PASS" : "FAIL");
                if (!buttonsOk) {
                    Fail("AO6 Ko-fi/GitHub region uniform");
                }
                const bool layoutOk = !isOnline || VerifyOnlineLayout(layout, dpi);
                if (isOnline) {
                    std::printf("check dpi=%d online-boxes=%s inside-plate=%s\n", dpi,
                                layoutOk ? "PASS" : "FAIL",
                                layoutOk ? "PASS" : "FAIL");
                    std::printf("check dpi=%d iphone-gaps=%s left=%ld right=%ld required=%d\n", dpi,
                                (RectValid(layout.iphone) &&
                                 layout.iphone.left - layout.text.right >= MulDiv(6, dpi, 96) &&
                                 layout.tile.left - layout.iphone.right >= MulDiv(6, dpi, 96))
                                    ? "PASS" : "FAIL",
                                layout.iphone.left - layout.text.right,
                                layout.tile.left - layout.iphone.right, MulDiv(6, dpi, 96));
                }
                if (!layoutOk) {
                    Fail("AO6 online layout boxes");
                }
                if (isOnline) {
                    const bool g1 = VerifyPlateRightEdge(layout, size.cx);
                    const bool g3 = VerifyQrMargin(layout, dpi);
                    const bool g4 = VerifyTagEmBaseline(layout);
                    std::printf("check dpi=%d G1-plate-right=%s right-gap=%ld\n", dpi,
                                g1 ? "PASS" : "FAIL",
                                static_cast<long>(size.cx - layout.plate.right));
                    std::printf("check dpi=%d G3-qr-margin=%s top=%ld bottom=%ld left=%ld right=%ld\n",
                                dpi, g3 ? "PASS" : "FAIL",
                                static_cast<long>(layout.tile.top - layout.plate.top),
                                static_cast<long>(layout.plate.bottom - layout.tile.bottom),
                                static_cast<long>(layout.tile.left - layout.plate.left),
                                static_cast<long>(layout.plate.right - layout.tile.right));
                    std::printf("check dpi=%d G4-em-baseline=%s row0=%d em0=%d row1=%d em1=%d\n",
                                dpi, g4 ? "PASS" : "FAIL", layout.tagBaseline[0],
                                layout.tagEmBaseline[0], layout.tagBaseline[1],
                                layout.tagEmBaseline[1]);
                    if (!g1) Fail("G1 online plate right edge");
                    if (!g3) Fail("G3 online QR tile margin");
                    if (!g4) Fail("G4 online em baseline");
                }
                if (!isOnline && !hover) {
                    int centre2 = 0;
                    const bool g2 = CopyCentered(capture, size.cx, size.cy, layout, &centre2);
                    std::printf("check dpi=%d G2-copy-centre=%s centre2=%d strip=%d\n", dpi,
                                g2 ? "PASS" : "FAIL", centre2, size.cy);
                    if (!g2) Fail("G2 DaGoat copy vertical centre");
                }
                if (!isOnline) {
                    const bool g5 = BurstOutsideFallbackText(capture, size.cx, layout);
                    std::printf("check dpi=%d G5-burst-text-clear=%s state=%s\n", dpi,
                                g5 ? "PASS" : "FAIL", hover ? "hover" : "rest");
                    if (!g5) Fail("G5 DaGoat burst or meteor crosses text");
                }
                if (isOnline && !hover) {
                    restLayout = layout;
                    haveRestLayout = true;
                }
                if (isOnline && hover && haveRestLayout) {
                    const bool stageGrew = (layout.stage.right - layout.stage.left) >
                                               (restLayout.stage.right - restLayout.stage.left) &&
                                           (layout.stage.bottom - layout.stage.top) >=
                                               (restLayout.stage.bottom - restLayout.stage.top);
                    const bool bracketsClosed = layout.brackets.left > restLayout.brackets.left &&
                                                layout.brackets.top > restLayout.brackets.top &&
                                                layout.brackets.right < restLayout.brackets.right &&
                                                layout.brackets.bottom < restLayout.brackets.bottom;
                    std::printf("check dpi=%d hover-stage=%s rest=%ldx%ld hover=%ldx%ld brackets=%s\n",
                                dpi, (stageGrew && bracketsClosed) ? "PASS" : "FAIL",
                                restLayout.stage.right - restLayout.stage.left,
                                restLayout.stage.bottom - restLayout.stage.top,
                                layout.stage.right - layout.stage.left,
                                layout.stage.bottom - layout.stage.top,
                                bracketsClosed ? "closed-inward" : "FAIL");
                    if (!stageGrew || !bracketsClosed) Fail("A4 hover stage/brackets");
                }
                if (isOnline && !hover) {
                    const bool qrOk = VerifyQr(capture, size.cx, online, layout);
                    std::printf("check dpi=%d QR-modules=%s\n", dpi, qrOk ? "PASS" : "FAIL");
                    if (!qrOk) Fail("AO6 QR rows");
                    if (dpi == 96) firstOnline = capture;
                }
                if (!isOnline && !hover) {
                    const double fraction = PlateNonBackground(capture, size.cx, layout.plate);
                    std::printf("dagoat dpi=%d non-background=%.4f\n", dpi, fraction);
                    if (fraction < 0.02) Fail("AO6 DaGoat plate too blank");
                    if (dpi == 96) firstDaGoat = capture;
                }
                if (!isOnline && !hover) {
                    const fs::path reference = output.parent_path() / "capture-ref71" /
                                               (std::string("ref71-") + std::to_string(dpi) +
                                                "-rest.png");
                    if (!fs::exists(reference)) {
                        Fail("DaGoat reference capture missing");
                    } else {
                        const double difference = ReferenceDifference(capture, size.cx, reference);
                        std::printf("reference dpi=%d pixel-difference=%.4f threshold=0.6500\n",
                                    dpi, difference);
                        if (difference >= 0.65) Fail("DaGoat reference difference too high");
                    }
                }
                DestroyWindow(hwnd);
            }
        }
    }
    if (!PixelsDiffer(firstDaGoat, firstOnline)) Fail("AO6 online and DaGoat pixels did not differ");
    cd::BannerHostSetModelForTest(nullptr);
    cd::SponsorSetDpiForTest(0);
    return g_fail == 0;
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path output = argc > 1 ? fs::path(argv[1]) : fs::path("build/capture-gdi");
    gp::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (gp::GdiplusStartup(&token, &input, nullptr) != gp::Ok) {
        Fail("GdiplusStartup");
    } else {
        ob::Model online;
        bool live = false;
        fs::path liveCache;
        fs::path messageJson;
        bool legacyMessageJson = false;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--live" && i + 1 < argc) {
                live = true;
                liveCache = fs::path(argv[++i]);
            } else if (arg == "--message-json" && i + 1 < argc) {
                messageJson = fs::path(argv[++i]);
            } else if (arg == "--legacy-message-json" && i + 1 < argc) {
                messageJson = fs::path(argv[++i]);
                legacyMessageJson = true;
            } else {
                Fail("unknown or incomplete capture option");
            }
        }
        if (live) {
            if (!FetchLive(liveCache, &online)) Fail("live banner fetch");
        } else {
            const fs::path jpgPath = fs::path("tests") / "banner-vectors" / "photos" /
                                     "hero-coffee-800.jpg";
            online = FixtureModel(ReadBytes(jpgPath));
            const fs::path signedPath = fs::path("tests") / "banner-vectors" / "cases" / "valid.signed";
            std::printf("fixture signed=%s bytes=%zu jpg=%zu\n", signedPath.string().c_str(),
                        ReadBytes(signedPath).size(), online.jpg.size());
        }
        if (online.jpg.empty()) Fail("fixture JPEG missing");
        if (!messageJson.empty()) {
            if (!WriteMessageJson(messageJson, online, legacyMessageJson))
                Fail("write message JSON");
        } else {
            CaptureAll(output, online);
        }
        gp::GdiplusShutdown(token);
    }
    std::printf("FAIL=%d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
