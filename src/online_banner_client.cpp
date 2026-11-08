#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <bcrypt.h>
#include <objidl.h>
#include <wincodec.h>
#include <winhttp.h>

#include "online_banner_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace ob {
namespace {

namespace fs = std::filesystem;

constexpr UINT kClientMessage = WM_APP + 0x4b;
constexpr UINT kRetryTimerMessage = WM_APP + 0x4c;
constexpr std::size_t kSignedCap = 16384u;
constexpr std::size_t kPhotoCap = 300000u;
constexpr DWORD kHeaderCap = 16384u;
constexpr int kSessionTimeoutMs = 10000;
constexpr wchar_t kWindowClass[] = L"GOATCamOnlineBannerClient";

std::int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string ReadBytes(const fs::path& path, std::size_t cap)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0 ||
        static_cast<unsigned long long>(size) >
            static_cast<unsigned long long>(cap) ||
        static_cast<unsigned long long>(size) >
            static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        return {};
    }
    input.seekg(0, std::ios::beg);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty() && !input.read(bytes.data(), static_cast<std::streamsize>(size))) {
        return {};
    }
    return bytes;
}

bool WriteBytes(const fs::path& path, const std::string& bytes)
{
    HANDLE output = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    std::size_t offset = 0;
    while (ok && offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        ok = WriteFile(output, bytes.data() + offset, chunk, &written, nullptr) != FALSE &&
             written == chunk;
        offset += written;
    }
    if (ok && FlushFileBuffers(output) == FALSE) ok = false;
    if (CloseHandle(output) == FALSE) ok = false;
    return ok;
}

bool IsLowerHex64(const std::string& text)
{
    if (text.size() != 64) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool ParseSafeInteger(const json::Value* value, std::int64_t* out)
{
    if (!value || value->type != json::Type::Number || value->text.empty()) {
        return false;
    }
    std::istringstream input(value->text);
    input.imbue(std::locale::classic());
    long double parsed = 0;
    input >> parsed;
    if (input.fail()) return false;
    const double number = static_cast<double>(parsed);
    constexpr double kMaxSafeInteger = 9007199254740991.0;
    if (!std::isfinite(number) || std::floor(number) != number ||
        std::fabs(number) > kMaxSafeInteger) {
        return false;
    }
    *out = static_cast<std::int64_t>(number);
    return true;
}

bool ParseBool(const json::Value* value, bool* out)
{
    if (!value || value->type != json::Type::Bool) return false;
    *out = value->b;
    return true;
}

bool ParseString(const json::Value* value, std::string* out)
{
    if (!value || value->type != json::Type::String) return false;
    *out = value->text;
    return true;
}

bool ParseRecord(const std::string& text, Record* out)
{
    json::Value root;
    if (json::Parse(text, 32, &root) != nullptr ||
        root.type != json::Type::Object) return false;

    Record record;
    std::int64_t parsedFailures = 0;
    const json::Value* hwm = root.Find("hwm");
    const json::Value* current = root.Find("current");
    const json::Value* lastCheck = root.Find("lastCheckMs");
    const json::Value* failures = root.Find("failures");
    if (!ParseSafeInteger(hwm, &record.hwm) || record.hwm < 0 || !current ||
        !ParseSafeInteger(lastCheck, &record.lastCheckMs) ||
        !ParseSafeInteger(failures, &parsedFailures) || parsedFailures < 0 ||
        parsedFailures > 3) {
        return false;
    }
    record.failures = static_cast<int>(parsedFailures);
    if (current->type == json::Type::Null) {
        record.hasCurrent = false;
        *out = record;
        return true;
    }
    if (current->type != json::Type::Object) return false;
    const json::Value* revision = current->Find("revision");
    const json::Value* sha256 = current->Find("sha256");
    const json::Value* enabled = current->Find("enabled");
    const json::Value* expiresMs = current->Find("expiresMs");
    const json::Value* photos = current->Find("photos");
    if (!ParseSafeInteger(revision, &record.current.revision) ||
        record.current.revision < 1 || !ParseString(sha256, &record.current.sha256) ||
        !IsLowerHex64(record.current.sha256) ||
        !ParseBool(enabled, &record.current.enabled) ||
        !ParseSafeInteger(expiresMs, &record.current.expiresMs) || !photos ||
        photos->type != json::Type::Object) {
        return false;
    }
    for (const auto& member : photos->members) {
        if (member.first != "webp" && member.first != "jpg") return false;
        std::string hash;
        if (!ParseString(&member.second, &hash) || !IsLowerHex64(hash)) return false;
        if (member.first == "webp") record.current.photoWebp = hash;
        else record.current.photoJpg = hash;
    }
    record.hasCurrent = true;
    *out = record;
    return true;
}

std::string RecordJson(const Record& record)
{
    std::ostringstream output;
    output << "{\"hwm\":" << record.hwm << ",\"current\":";
    if (!record.hasCurrent) {
        output << "null";
    } else {
        output << "{\"revision\":" << record.current.revision
               << ",\"sha256\":\"" << record.current.sha256
               << "\",\"enabled\":" << (record.current.enabled ? "true" : "false")
               << ",\"expiresMs\":" << record.current.expiresMs
               << ",\"photos\":{";
        bool comma = false;
        if (!record.current.photoWebp.empty()) {
            output << "\"webp\":\"" << record.current.photoWebp << '\"';
            comma = true;
        }
        if (!record.current.photoJpg.empty()) {
            if (comma) output << ',';
            output << "\"jpg\":\"" << record.current.photoJpg << '\"';
        }
        output << "}}";
    }
    output << ",\"lastCheckMs\":" << record.lastCheckMs
           << ",\"failures\":" << record.failures << '}';
    return output.str();
}

bool AtomicWrite(const fs::path& path, const std::string& bytes)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    const fs::path temporary = path.wstring() + L".tmp";
    if (!WriteBytes(temporary, bytes)) return false;
    return MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::wstring WidenAscii(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

std::wstring RequestPath(const Config& config, const std::string& file)
{
    std::wstring prefix = config.pathPrefix;
    if (!prefix.empty() && prefix.front() != L'/') prefix.insert(prefix.begin(), L'/');
    while (prefix.size() > 1 && prefix.back() == L'/') prefix.pop_back();
    return prefix + L"/banner/" + WidenAscii(file);
}

bool DecodeJpegSize(const std::string& bytes, int expectedWidth, int expectedHeight)
{
    if (bytes.empty()) return false;
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(init);
    IWICImagingFactory* factory = nullptr;
    IStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    bool result = false;

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!memory) goto done;
    {
        void* destination = GlobalLock(memory);
        if (!destination) {
            GlobalFree(memory);
            goto done;
        }
        memcpy(destination, bytes.data(), bytes.size());
        GlobalUnlock(memory);
    }
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
        GlobalFree(memory);
        goto done;
    }
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame))) {
        goto done;
    }
    {
        UINT width = 0;
        UINT height = 0;
        result = SUCCEEDED(frame->GetSize(&width, &height)) &&
                 width == static_cast<UINT>(expectedWidth) &&
                 height == static_cast<UINT>(expectedHeight);
    }

done:
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (uninitialize) CoUninitialize();
    return result;
}

bool ModelsEqual(const Model& a, const Model& b)
{
    if (a.online != b.online || a.webp != b.webp || a.jpg != b.jpg) return false;
    if (!a.online) return true;
    return a.banner.revision == b.banner.revision &&
           a.banner.link == b.banner.link &&
           a.banner.photoWebp == b.banner.photoWebp &&
           a.banner.photoJpg == b.banner.photoJpg;
}

} // namespace

struct Client::Impl {
    struct StoredSet {
        bool valid = false;
        std::string signedBytes;
        Result result;
        std::string webp;
        std::string jpg;
    };

    struct Attempt {
        explicit Attempt(std::int64_t attemptHwm, std::string stored)
            : hwm(attemptHwm), storedSigned(std::move(stored)),
              cancelEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr))
        {
        }

        ~Attempt()
        {
            CloseHandles();
            if (cancelEvent) CloseHandle(cancelEvent);
        }

        void SetHandle(int index, HINTERNET handle)
        {
            HINTERNET closeNow = nullptr;
            {
                std::lock_guard<std::mutex> lock(handleMutex);
                if (cancelled.load()) closeNow = handle;
                else handles[static_cast<std::size_t>(index)] = handle;
            }
            if (closeNow) WinHttpCloseHandle(closeNow);
        }

        void CloseHandles()
        {
            std::array<HINTERNET, 3> toClose{};
            {
                std::lock_guard<std::mutex> lock(handleMutex);
                toClose = handles;
                handles = {};
            }
            for (HINTERNET handle : toClose) {
                if (handle) WinHttpCloseHandle(handle);
            }
        }

        void Cancel()
        {
            cancelled.store(true);
            if (cancelEvent) SetEvent(cancelEvent);
            CloseHandles();
        }

        const std::int64_t hwm;
        const std::string storedSigned;
        std::int64_t gen = 0;
        std::atomic<bool> cancelled{false};
        HANDLE cancelEvent = nullptr;
        std::mutex handleMutex;
        std::array<HINTERNET, 3> handles{};
        std::string downloadedSigned;
        Result result;
        std::string webp;
        std::string jpg;
    };

    enum class CompletionType { Signed, Photos, Failed };

    struct Completion {
        CompletionType type = CompletionType::Failed;
        std::int64_t gen = 0;
        bool ok = false;
        std::shared_ptr<Attempt> attempt;
    };

    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    Client* owner;
    Config config;
    HWND notify;
    UINT notifyMsg;
    HWND window = nullptr;
    UINT_PTR timerId = 0;
    Machine machine;
    mutable std::mutex modelMutex;
    Model model;
    StoredSet currentSet;
    std::shared_ptr<Attempt> currentAttempt;
    std::vector<Worker> workers;
    std::atomic<std::int64_t> runningGen{0};
    std::atomic<bool> shuttingDown{false};
    bool pendingArm = false;
    std::int64_t pendingArmAtMs = 0;
    std::string clickUrl;
    std::mutex parkedMutex;
    std::deque<std::unique_ptr<Completion>> parkedCompletions;

    Impl(Client* client, Config value, HWND target, UINT message)
        : owner(client), config(std::move(value)), notify(target), notifyMsg(message),
          timerId(NextTimerId()), machine(config.floor, config.kinds)
    {
        if (!config.allowInsecureForTests &&
            (!config.https || !config.pathPrefix.empty())) {
            ReportError(L"GOATCam online banner: insecure production configuration\n");
            return;
        }
        RegisterWindowClass();
        window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
                                 this);
        if (!window) {
            ReportError(L"GOATCam online banner: message window creation failed; fetching disabled\n");
        }
        Startup();
    }

    ~Impl()
    {
        if (machine.GetState().visible) {
            Event hidden;
            hidden.type = EventType::Hidden;
            DispatchEvent(hidden);
        }
        shuttingDown.store(true);
        runningGen.store(0);
        if (currentAttempt) currentAttempt->Cancel();
        if (window) KillTimer(window, timerId);
        for (Worker& worker : workers) {
            if (worker.thread.joinable()) worker.thread.join();
        }
        if (window) {
            MSG message{};
            while (PeekMessageW(&message, window, kClientMessage,
                                kClientMessage, PM_REMOVE)) {
                delete reinterpret_cast<Completion*>(message.wParam);
            }
            DestroyWindow(window);
            window = nullptr;
        }
    }

    static UINT_PTR NextTimerId()
    {
        static std::atomic<UINT_PTR> next{1};
        UINT_PTR id = next.fetch_add(1);
        if (id == 0) id = next.fetch_add(1);
        return id;
    }

    static void RegisterWindowClass()
    {
        static std::once_flag once;
        std::call_once(once, []() {
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = &WindowProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = kWindowClass;
            RegisterClassExW(&windowClass);
        });
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message,
                                       WPARAM wParam, LPARAM lParam)
    {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
        self->RetryPendingArm();
        if (message == WM_TIMER && wParam == self->timerId) {
            self->DrainParkedCompletions();
            self->owner->TimerFired();
            return 0;
        }
        if (message == kRetryTimerMessage && wParam == self->timerId) {
            self->DrainParkedCompletions();
            return 0;
        }
        if (message == kClientMessage) {
            self->OnCompletion(reinterpret_cast<Completion*>(wParam));
            return 0;
        }
        if (message == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool IsActive(const std::shared_ptr<Attempt>& attempt, std::int64_t gen) const
    {
        return !shuttingDown.load() && runningGen.load() == gen && attempt &&
               !attempt->cancelled.load();
    }

    bool CanFetch() const
    {
        return window != nullptr && !shuttingDown.load();
    }

    void ReapFinishedWorkers()
    {
        for (auto worker = workers.begin(); worker != workers.end();) {
            if (!worker->finished->load(std::memory_order_acquire)) {
                ++worker;
                continue;
            }
            if (worker->thread.joinable()) worker->thread.join();
            worker = workers.erase(worker);
        }
    }

    void SetModel(const Model& next)
    {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(modelMutex);
            changed = !ModelsEqual(model, next);
            model = next;
        }
        if (changed && notify) PostMessageW(notify, notifyMsg, 0, 0);
    }

    Model GetModel() const
    {
        std::lock_guard<std::mutex> lock(modelMutex);
        return model;
    }

    void ReportError(const wchar_t* message)
    {
        OutputDebugStringW(message);
        if (notify) PostMessageW(notify, notifyMsg, 0, 0);
    }

    std::int64_t TimerDelay(std::int64_t atMs) const
    {
        const std::int64_t delta = atMs - NowMs();
        return std::max<std::int64_t>(0, delta);
    }

    bool TrySetTimer(std::int64_t atMs)
    {
        const DWORD delay = static_cast<DWORD>(std::min<std::int64_t>(
            TimerDelay(atMs), std::numeric_limits<DWORD>::max()));
        if (SetTimer(window, timerId, delay, nullptr) != 0) return true;
        ReportError(L"GOATCam online banner: SetTimer failed\n");
        return false;
    }

    void RetryPendingArm()
    {
        if (!pendingArm || !window || shuttingDown.load()) return;
        if (TrySetTimer(pendingArmAtMs)) {
            pendingArm = false;
            pendingArmAtMs = 0;
        }
    }

    void ArmTimer(std::int64_t atMs)
    {
        if (!window) return;
        pendingArm = false;
        pendingArmAtMs = 0;
        KillTimer(window, timerId);
        // ponytail: the machine re-arms on every TIMER, so the 49-day DWORD clamp simply fires early and re-arms.
        if (TrySetTimer(atMs)) return;
        pendingArm = true;
        pendingArmAtMs = atMs;
        if (!PostMessageW(
                window, kRetryTimerMessage, static_cast<WPARAM>(timerId), 0)) {
            ReportError(L"GOATCam online banner: timer retry post failed\n");
        }
    }

    void Startup()
    {
        Record record;
        bool hasRecord = false;
        const std::string recordBytes = ReadBytes(
            fs::path(config.cacheDir) / "record.json", kSignedCap);
        if (!recordBytes.empty() && ParseRecord(recordBytes, &record)) hasRecord = true;
        currentSet = VerifyStoredSet(hasRecord ? &record : nullptr);

        Event start;
        start.type = EventType::Start;
        start.hasRecord = hasRecord;
        start.record = record;
        start.setValid = currentSet.valid;
        DispatchEvent(start);
    }

    StoredSet VerifyStoredSet(const Record* record)
    {
        StoredSet stored;
        if (!record || !record->hasCurrent) return stored;
        const fs::path setDir = fs::path(config.cacheDir) / "sets" /
                                WidenAscii(record->current.sha256);
        const std::string signedBytes = ReadBytes(setDir / "v1.signed", kSignedCap);
        if (signedBytes.empty()) return stored;
        Context context;
        context.publicKey65 = config.publicKey65;
        context.nowMs = NowMs();
        context.hwm = record->hwm;
        context.floor = config.floor;
        context.storedSigned = &signedBytes;
        const Result result = Evaluate(signedBytes, context);
        if ((result.action != Action::Show && result.action != Action::KillSwitch) ||
            result.sha256 != record->current.sha256 ||
            result.banner.revision != record->current.revision ||
            record->current.enabled != (result.action == Action::Show) ||
            record->current.expiresMs != result.banner.expiresMs) {
            return stored;
        }
        const bool drawsWebp = std::find(config.kinds.begin(), config.kinds.end(),
                                        Kind::Webp) != config.kinds.end();
        const bool drawsJpg = std::find(config.kinds.begin(), config.kinds.end(),
                                        Kind::Jpg) != config.kinds.end();
        if ((result.action == Action::Show &&
             ((drawsWebp && record->current.photoWebp != result.banner.sha256Webp) ||
              (!drawsWebp && !record->current.photoWebp.empty()) ||
              (drawsJpg && record->current.photoJpg != result.banner.sha256Jpg) ||
              (!drawsJpg && !record->current.photoJpg.empty()))) ||
            (result.action == Action::KillSwitch &&
             (!record->current.photoWebp.empty() ||
              !record->current.photoJpg.empty()))) {
            return stored;
        }
        std::string webp;
        std::string jpg;
        if (result.action == Action::Show) {
            for (Kind kind : config.kinds) {
                const bool isWebp = kind == Kind::Webp;
                const std::string bytes = ReadBytes(
                    setDir / (isWebp ? "photo.webp" : "photo.jpg"), kPhotoCap);
                const std::string& expected = isWebp ? result.banner.sha256Webp
                                                     : result.banner.sha256Jpg;
                if (CheckPhoto(bytes, !isWebp, expected, result.banner.w,
                               result.banner.h) != nullptr) {
                    return stored;
                }
                if (isWebp) webp = bytes;
                else jpg = bytes;
            }
            if (std::find(config.kinds.begin(), config.kinds.end(), Kind::Jpg) !=
                    config.kinds.end()) {
                bool decoded = false;
                std::thread decoder([&]() {
                    decoded = DecodeJpegSize(jpg, result.banner.w, result.banner.h);
                });
                decoder.join();
                if (!decoded) return stored;
            }
        }
        stored.valid = true;
        stored.signedBytes = signedBytes;
        stored.result = result;
        stored.webp = std::move(webp);
        stored.jpg = std::move(jpg);
        return stored;
    }

    void DispatchEvent(const Event& event)
    {
        const Machine::State before = machine.GetState();
        const std::vector<Effect> effects = machine.Dispatch(event, NowMs());
        if (event.type == EventType::Committed && before.attempting &&
            before.gen == event.gen && currentAttempt &&
            currentAttempt->gen == event.gen) {
            currentSet.valid = true;
            currentSet.signedBytes = currentAttempt->downloadedSigned;
            currentSet.result = currentAttempt->result;
            currentSet.webp = currentAttempt->webp;
            currentSet.jpg = currentAttempt->jpg;
        }
        ProcessEffects(effects);
    }

    void ProcessEffects(const std::vector<Effect>& effects)
    {
        for (const Effect& effect : effects) ProcessEffect(effect);
    }

    void ProcessEffect(const Effect& effect)
    {
        switch (effect.type) {
        case EffectType::FetchSigned:
            StartSigned(effect.gen);
            break;
        case EffectType::FetchPhotos:
            StartPhotos(effect.gen);
            break;
        case EffectType::Cancel:
            if (currentAttempt && currentAttempt->gen == effect.gen) {
                runningGen.store(0);
                currentAttempt->Cancel();
                currentAttempt.reset();
            }
            break;
        case EffectType::WriteSet: {
            if (!currentAttempt || !IsActive(currentAttempt, effect.gen)) break;
            const bool ok = WriteSet(effect.gen, *currentAttempt, effect.sha256);
            if (!IsActive(currentAttempt, effect.gen)) break;
            Event completion;
            completion.type = ok ? EventType::Committed : EventType::Failed;
            completion.gen = effect.gen;
            DispatchEvent(completion);
            break;
        }
        case EffectType::WriteRecord:
            (void)AtomicWrite(fs::path(config.cacheDir) / "record.json",
                              RecordJson(effect.record));
            break;
        case EffectType::DeleteUnnamedSets:
            DeleteUnnamedSets();
            break;
        case EffectType::Display: {
            Model next;
            if (effect.online && currentSet.valid &&
                currentSet.result.action == Action::Show) {
                next.online = true;
                next.banner = currentSet.result.banner;
                next.webp = currentSet.webp;
                next.jpg = currentSet.jpg;
            }
            SetModel(next);
            break;
        }
        case EffectType::ArmTimer: {
            ArmTimer(effect.atMs);
            break;
        }
        case EffectType::DisarmTimer:
            pendingArm = false;
            pendingArmAtMs = 0;
            if (window) KillTimer(window, timerId);
            break;
        case EffectType::OpenLink: {
            const Model current = GetModel();
            clickUrl = current.online ? current.banner.link :
                                       "https://dagoat.io";
            break;
        }
        }
    }

    bool WriteSet(std::int64_t gen, const Attempt& attempt,
                  const std::string& sha256)
    {
        const fs::path setDir = fs::path(config.cacheDir) / "sets" /
                                WidenAscii(sha256);
        std::error_code error;
        fs::create_directories(setDir, error);
        if (error) return false;
        if (!AtomicWrite(setDir / "v1.signed", attempt.downloadedSigned)) return false;
        if (std::find(config.kinds.begin(), config.kinds.end(), Kind::Webp) !=
                config.kinds.end() &&
            !AtomicWrite(setDir / "photo.webp", attempt.webp)) return false;
        if (std::find(config.kinds.begin(), config.kinds.end(), Kind::Jpg) !=
                config.kinds.end() &&
            !AtomicWrite(setDir / "photo.jpg", attempt.jpg)) return false;
        (void)gen;
        return true;
    }

    void DeleteUnnamedSets()
    {
        const fs::path sets = fs::path(config.cacheDir) / "sets";
        std::error_code error;
        if (!fs::is_directory(sets, error)) return;
        const Record record = machine.GetState().record;
        const std::wstring keep = record.hasCurrent
                                      ? WidenAscii(record.current.sha256)
                                      : std::wstring();
        for (const fs::directory_entry& entry : fs::directory_iterator(sets, error)) {
            if (error) return;
            std::error_code entryError;
            if (entry.is_directory(entryError) &&
                entry.path().filename().wstring() != keep) {
                fs::remove_all(entry.path(), entryError);
            }
        }
    }

    void StartSigned(std::int64_t gen)
    {
        ReapFinishedWorkers();
        const Machine::State state = machine.GetState();
        auto attempt = std::make_shared<Attempt>(state.record.hwm,
                                                 currentSet.signedBytes);
        attempt->gen = gen;
        currentAttempt = attempt;
        runningGen.store(gen);
        const auto finished = std::make_shared<std::atomic<bool>>(false);
        std::thread worker([this, attempt, gen, finished]() {
            RunSigned(attempt, gen);
            finished->store(true, std::memory_order_release);
        });
        workers.push_back(Worker{std::move(worker), finished});
    }

    void StartPhotos(std::int64_t gen)
    {
        ReapFinishedWorkers();
        const std::shared_ptr<Attempt> attempt = currentAttempt;
        if (!attempt || attempt->gen != gen || !IsActive(attempt, gen)) return;
        const auto finished = std::make_shared<std::atomic<bool>>(false);
        std::thread worker([this, attempt, gen, finished]() {
            RunPhotos(attempt, gen);
            finished->store(true, std::memory_order_release);
        });
        workers.push_back(Worker{std::move(worker), finished});
    }

    void RunSigned(const std::shared_ptr<Attempt>& attempt, std::int64_t gen)
    {
        std::string bytes;
        if (!FetchBytes(RequestPath(config, "v1.signed"), kSignedCap, attempt,
                        &bytes, gen)) {
            if (IsActive(attempt, gen)) PostCompletion(CompletionType::Failed, gen,
                                                        false, attempt);
            return;
        }
        if (!IsActive(attempt, gen)) return;
        Context context;
        context.publicKey65 = config.publicKey65;
        context.nowMs = NowMs();
        context.hwm = attempt->hwm;
        context.floor = config.floor;
        context.storedSigned = attempt->storedSigned.empty()
                                   ? nullptr
                                   : &attempt->storedSigned;
        const Result result = Evaluate(bytes, context);
        if (!IsActive(attempt, gen)) return;
        attempt->downloadedSigned = std::move(bytes);
        attempt->result = result;
        PostCompletion(CompletionType::Signed, gen, true, attempt);
    }

    void RunPhotos(const std::shared_ptr<Attempt>& attempt, std::int64_t gen)
    {
        const Result result = attempt->result;
        for (Kind kind : config.kinds) {
            if (!IsActive(attempt, gen)) return;
            const bool isWebp = kind == Kind::Webp;
            const std::string& file = isWebp ? result.banner.photoWebp
                                             : result.banner.photoJpg;
            std::string bytes;
            if (!FetchBytes(RequestPath(config, file), kPhotoCap, attempt,
                            &bytes, gen)) {
                if (IsActive(attempt, gen)) {
                    PostCompletion(CompletionType::Photos, gen, false, attempt);
                }
                return;
            }
            if (!IsActive(attempt, gen)) return;
            const std::string& hash = isWebp ? result.banner.sha256Webp
                                             : result.banner.sha256Jpg;
            if (CheckPhoto(bytes, !isWebp, hash, result.banner.w,
                           result.banner.h) != nullptr) {
                if (IsActive(attempt, gen)) {
                    PostCompletion(CompletionType::Photos, gen, false, attempt);
                }
                return;
            }
            if (!IsActive(attempt, gen)) return;
            if (!isWebp) {
                if (!DecodeJpegSize(bytes, result.banner.w, result.banner.h)) {
                    if (IsActive(attempt, gen)) {
                        PostCompletion(CompletionType::Photos, gen, false, attempt);
                    }
                    return;
                }
            }
            if (!IsActive(attempt, gen)) return;
            if (isWebp) attempt->webp = std::move(bytes);
            else attempt->jpg = std::move(bytes);
        }
        if (IsActive(attempt, gen)) PostCompletion(CompletionType::Photos, gen,
                                                    true, attempt);
    }

    void PostCompletion(CompletionType type, std::int64_t gen, bool ok,
                        const std::shared_ptr<Attempt>& attempt)
    {
        if (!IsActive(attempt, gen)) return;
        auto completion = std::make_unique<Completion>();
        completion->type = type;
        completion->gen = gen;
        completion->ok = ok;
        completion->attempt = attempt;
        if (window && PostMessageW(window, kClientMessage,
                                   reinterpret_cast<WPARAM>(completion.get()), 0)) {
            completion.release();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(parkedMutex);
            if (shuttingDown.load()) return;
            parkedCompletions.push_back(std::move(completion));
        }
        ReportError(L"GOATCam online banner: completion post failed; completion parked\n");
    }

    void DrainParkedCompletions()
    {
        std::deque<std::unique_ptr<Completion>> parked;
        {
            std::lock_guard<std::mutex> lock(parkedMutex);
            parked.swap(parkedCompletions);
        }
        while (!parked.empty()) {
            std::unique_ptr<Completion> completion = std::move(parked.front());
            parked.pop_front();
            OnCompletion(completion.release());
        }
    }

    void OnCompletion(Completion* raw)
    {
        std::unique_ptr<Completion> completion(raw);
        if (!completion || !IsActive(completion->attempt, completion->gen)) {
            ReapFinishedWorkers();
            return;
        }
        if (completion->type == CompletionType::Signed) {
            Event event;
            event.type = EventType::Signed;
            event.gen = completion->gen;
            event.result = completion->attempt->result;
            DispatchEvent(event);
        } else if (completion->type == CompletionType::Photos) {
            Event event;
            event.type = EventType::Photos;
            event.gen = completion->gen;
            event.ok = completion->ok;
            DispatchEvent(event);
        } else {
            Event event;
            event.type = EventType::Failed;
            event.gen = completion->gen;
            DispatchEvent(event);
        }
        ReapFinishedWorkers();
    }

    bool FetchBytes(const std::wstring& path, std::size_t cap,
                    const std::shared_ptr<Attempt>& attempt,
                    std::string* output, std::int64_t gen)
    {
        if (!IsActive(attempt, gen)) return false;
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!done) return false;
        std::atomic<bool> timedOut{false};
        std::thread watchdog([&]() {
            HANDLE waits[2] = {attempt->cancelEvent, done};
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE,
                                                      config.deadlineMs);
            if (wait == WAIT_TIMEOUT) {
                timedOut.store(true);
                attempt->CloseHandles();
            }
        });
        auto finish = [&]() {
            SetEvent(done);
            watchdog.join();
            attempt->CloseHandles();
            CloseHandle(done);
        };
        auto active = [&]() {
            return IsActive(attempt, gen) && !timedOut.load();
        };
        auto setHandle = [&](int slot, HINTERNET handle) {
            attempt->SetHandle(slot, handle);
            if (!active()) attempt->CloseHandles();
            return active();
        };

        HINTERNET session = WinHttpOpen(
            config.userAgent.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session || !setHandle(0, session)) {
            finish();
            return false;
        }
        if (!WinHttpSetTimeouts(session, kSessionTimeoutMs, kSessionTimeoutMs,
                               kSessionTimeoutMs, kSessionTimeoutMs) || !active()) {
            finish();
            return false;
        }
        HINTERNET connect = WinHttpConnect(session, config.host.c_str(),
                                           config.port, 0);
        if (!connect || !setHandle(1, connect)) {
            finish();
            return false;
        }
        const DWORD flags = config.https ? WINHTTP_FLAG_SECURE : 0;
        const wchar_t* acceptTypes[] = {L"*/*", nullptr};
        HINTERNET request = WinHttpOpenRequest(
            connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
            acceptTypes, flags);
        if (!request || !setHandle(2, request)) {
            finish();
            return false;
        }
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        DWORD features = WINHTTP_DISABLE_COOKIES;
        DWORD autoLogon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        DWORD headerCap = kHeaderCap;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
                              &redirectPolicy, sizeof(redirectPolicy)) ||
            !WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &features,
                              sizeof(features)) ||
            !WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autoLogon,
                              sizeof(autoLogon)) ||
            !WinHttpSetOption(request, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE,
                              &headerCap, sizeof(headerCap)) ||
            !active()) {
            finish();
            return false;
        }
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !active() || !WinHttpReceiveResponse(request, nullptr) || !active()) {
            finish();
            return false;
        }
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(
                request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                WINHTTP_NO_HEADER_INDEX) || status != 200 || !active()) {
            finish();
            return false;
        }
        DWORD encodingSize = 0;
        DWORD encodingIndex = WINHTTP_NO_HEADER_INDEX;
        const BOOL encodingFirst = WinHttpQueryHeaders(
            request, WINHTTP_QUERY_CONTENT_ENCODING,
            WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &encodingSize,
            &encodingIndex);
        const DWORD encodingError = GetLastError();
        if (encodingFirst || encodingError == ERROR_INSUFFICIENT_BUFFER) {
            finish();
            return false;
        }
        if (encodingError != ERROR_WINHTTP_HEADER_NOT_FOUND) {
            finish();
            return false;
        }
        DWORD contentLength = 0;
        DWORD contentLengthSize = sizeof(contentLength);
        const BOOL hasLength = WinHttpQueryHeaders(
            request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthSize,
            WINHTTP_NO_HEADER_INDEX);
        if (hasLength && contentLength > cap) {
            finish();
            return false;
        }
        if (!hasLength && GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND) {
            finish();
            return false;
        }

        std::string bytes;
        for (;;) {
            if (!active()) {
                finish();
                return false;
            }
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                finish();
                return false;
            }
            if (available == 0) break;
            const DWORD want = std::min<DWORD>(available, 65536u);
            std::string chunk(want, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(request, chunk.data(), want, &got) || got == 0 ||
                bytes.size() > cap - std::min<std::size_t>(cap, got)) {
                finish();
                return false;
            }
            bytes.append(chunk.data(), got);
            if (bytes.size() > cap) {
                finish();
                return false;
            }
        }
        if (!active()) {
            finish();
            return false;
        }
        *output = std::move(bytes);
        finish();
        return true;
    }
};

Client::Client(Config config, HWND notify, UINT notifyMsg)
    : impl_(std::make_unique<Impl>(this, std::move(config), notify, notifyMsg))
{
}

Client::~Client() = default;

void Client::Shown()
{
    impl_->RetryPendingArm();
    impl_->DrainParkedCompletions();
    Event event;
    event.type = EventType::Shown;
    event.mayFetch = impl_->CanFetch();
    impl_->DispatchEvent(event);
}

void Client::Hidden()
{
    impl_->RetryPendingArm();
    impl_->DrainParkedCompletions();
    Event event;
    event.type = EventType::Hidden;
    impl_->DispatchEvent(event);
}

void Client::Pointer(bool over)
{
    impl_->RetryPendingArm();
    impl_->DrainParkedCompletions();
    Event event;
    event.type = EventType::Pointer;
    event.over = over;
    impl_->DispatchEvent(event);
}

void Client::Focus(bool inside)
{
    impl_->RetryPendingArm();
    impl_->DrainParkedCompletions();
    Event event;
    event.type = EventType::Focus;
    event.inside = inside;
    impl_->DispatchEvent(event);
}

bool Client::Click(std::string* urlToOpen)
{
    impl_->RetryPendingArm();
    impl_->DrainParkedCompletions();
    impl_->clickUrl.clear();
    Event event;
    event.type = EventType::Click;
    impl_->DispatchEvent(event);
    if (impl_->clickUrl.empty()) return false;
    if (urlToOpen) *urlToOpen = impl_->clickUrl;
    return true;
}

void Client::TimerFired()
{
    impl_->RetryPendingArm();
    impl_->DrainParkedCompletions();
    Event event;
    event.type = EventType::Timer;
    event.mayFetch = impl_->CanFetch();
    impl_->DispatchEvent(event);
}

void Client::Resumed()
{
    impl_->RetryPendingArm();
    impl_->DrainParkedCompletions();
    Event event;
    event.type = EventType::Resume;
    event.mayFetch = impl_->CanFetch();
    impl_->DispatchEvent(event);
}

Model Client::Current() const
{
    impl_->RetryPendingArm();
    impl_->DrainParkedCompletions();
    return impl_->GetModel();
}

UINT_PTR Client::TimerId() const
{
    impl_->RetryPendingArm();
    impl_->DrainParkedCompletions();
    return impl_->timerId;
}

} // namespace ob
