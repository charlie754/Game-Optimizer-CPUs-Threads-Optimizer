#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <bcrypt.h>

#include "online_banner.h"
#include "online_banner_client.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<std::uint8_t>;

std::int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Bytes ReadBytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) return {};
    input.seekg(0, std::ios::beg);
    Bytes bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return {};
    }
    return bytes;
}

bool WriteBytes(const fs::path& path, const Bytes& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    return output.good();
}

bool WriteText(const fs::path& path, const std::string& text)
{
    return WriteBytes(path, Bytes(text.begin(), text.end()));
}

Bytes Sha256(const Bytes& bytes)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Bytes digest(32);
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return {};
    }
    ULONG objectLength = 0;
    ULONG resultLength = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
            &resultLength, 0)) ||
        !BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0,
                                         nullptr, 0, 0)) ||
        !BCRYPT_SUCCESS(BCryptHashData(hash,
                                       const_cast<PUCHAR>(bytes.data()),
                                       static_cast<ULONG>(bytes.size()), 0)) ||
        !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(),
                                         static_cast<ULONG>(digest.size()), 0))) {
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    (void)objectLength;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest;
}

std::string Hex(const Bytes& bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (std::uint8_t byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

std::string Base64(const Bytes& bytes)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const std::size_t remaining = bytes.size() - offset;
        const std::uint32_t value =
            (static_cast<std::uint32_t>(bytes[offset]) << 16) |
            (remaining > 1 ? static_cast<std::uint32_t>(bytes[offset + 1]) << 8 : 0) |
            (remaining > 2 ? static_cast<std::uint32_t>(bytes[offset + 2]) : 0);
        result.push_back(alphabet[(value >> 18) & 0x3f]);
        result.push_back(alphabet[(value >> 12) & 0x3f]);
        result.push_back(remaining > 1 ? alphabet[(value >> 6) & 0x3f] : '=');
        result.push_back(remaining > 2 ? alphabet[value & 0x3f] : '=');
    }
    return result;
}

struct KeyMaterial {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    std::uint8_t publicKey[65]{};

    ~KeyMaterial()
    {
        if (key) BCryptDestroyKey(key);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

bool MakeKey(KeyMaterial* material)
{
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &material->algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)) ||
        !BCRYPT_SUCCESS(BCryptGenerateKeyPair(material->algorithm, &material->key,
                                              256, 0)) ||
        !BCRYPT_SUCCESS(BCryptFinalizeKeyPair(material->key, 0))) {
        return false;
    }
    ULONG blobSize = 0;
    ULONG resultSize = 0;
    if (!BCRYPT_SUCCESS(BCryptExportKey(
            material->key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0,
            &blobSize, 0))) {
        return false;
    }
    Bytes blob(blobSize);
    if (!BCRYPT_SUCCESS(BCryptExportKey(
            material->key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), blobSize,
            &resultSize, 0)) || resultSize < sizeof(BCRYPT_ECCKEY_BLOB) + 64) {
        return false;
    }
    material->publicKey[0] = 0x04;
    std::copy_n(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), 64,
                material->publicKey + 1);
    return true;
}

Bytes Sign(KeyMaterial* material, const std::string& json)
{
    const std::string message = "GOATCAM-BANNER-V1\n" + json;
    const Bytes messageBytes(message.begin(), message.end());
    const Bytes digest = Sha256(messageBytes);
    if (digest.size() != 32) return {};
    Bytes signature(64);
    ULONG signatureSize = 0;
    if (!BCRYPT_SUCCESS(BCryptSignHash(
            material->key, nullptr, const_cast<PUCHAR>(digest.data()),
            static_cast<ULONG>(digest.size()), signature.data(),
            static_cast<ULONG>(signature.size()), &signatureSize, 0)) ||
        signatureSize != signature.size()) {
        return {};
    }
    return signature;
}

std::string Iso(std::int64_t ms)
{
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    char text[32];
    sprintf_s(text, "%04d-%02d-%02dT%02d:%02d:%02dZ",
              utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
              utc.tm_hour, utc.tm_min, utc.tm_sec);
    return text;
}

struct Fixture {
    std::string json;
    Bytes signedBytes;
    std::string sha256;
    std::int64_t revision = 0;
};

Fixture MakeFixture(KeyMaterial* material, const Bytes& webp, const Bytes& jpg,
                    std::int64_t revision)
{
    const std::string keyId = ob::KeyIdOf(material->publicKey);
    const std::string webpHash = Hex(Sha256(webp));
    const std::string jpgHash = Hex(Sha256(jpg));
    const std::int64_t now = NowMs();
    std::ostringstream json;
    json << "{\"schema\":1,\"keyId\":\"" << keyId
         << "\",\"revision\":" << revision
         << ",\"issued\":\"" << Iso(now - 60000)
         << "\",\"expires\":\"" << Iso(now + 86400000)
         << "\",\"enabled\":true"
         << ",\"headline\":\"GOAT Cam online\""
         << ",\"wordmark\":\"GOAT Cam\""
         << ",\"tagline\":[\"iPhone app\"]"
         << ",\"effects\":{\"viewfinder\":true,\"steam\":true}"
         << ",\"link\":\"https://goatcam.dagoat.io\""
         << ",\"qr\":{\"text\":\"https://apps.apple.com/app/id6816268316\",\"size\":37,\"rows\":[";
    for (int row = 0; row < 37; ++row) {
        if (row != 0) json << ',';
        json << '\"' << std::string(37, '0') << '\"';
    }
    json << "]}"
         << ",\"photo\":{\"webp\":\"hero-coffee-800.webp\",\"jpg\":\"hero-coffee-800.jpg\""
         << ",\"sha256\":{\"webp\":\"" << webpHash
         << "\",\"jpg\":\"" << jpgHash << "\"}"
         << ",\"w\":800,\"h\":498,\"focus\":[0.3,0.55]"
         << ",\"steamAt\":[0.17,0.62],\"textBox\":[0.33,0.1,0.4,0.45]}"
         << ",\"labels\":{\"link\":\"Open GOAT Cam\",\"qr\":\"GOAT Cam App Store QR\"}}";
    Fixture fixture;
    fixture.json = json.str();
    const Bytes signature = Sign(material, fixture.json);
    const std::string signedText = Base64(signature) + "\n" + fixture.json;
    fixture.signedBytes.assign(signedText.begin(), signedText.end());
    fixture.sha256 = Hex(Sha256(fixture.signedBytes));
    fixture.revision = revision;
    return fixture;
}

std::string StoredRecord(const Fixture& fixture, const Bytes& webp,
                         const Bytes& jpg, std::int64_t expiresMs)
{
    return "{\"hwm\":" + std::to_string(fixture.revision) +
           ",\"current\":{\"revision\":" +
           std::to_string(fixture.revision) + ",\"sha256\":\"" +
           fixture.sha256 + "\",\"enabled\":true,\"expiresMs\":" +
           std::to_string(expiresMs) + ",\"photos\":{\"webp\":\"" +
           Hex(Sha256(webp)) + "\",\"jpg\":\"" + Hex(Sha256(jpg)) +
           "\"}},\"lastCheckMs\":" + std::to_string(NowMs()) +
           ",\"failures\":0}";
}

fs::path MakeTemp(const wchar_t* name)
{
    wchar_t tempPath[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, tempPath);
    if (length == 0 || length >= MAX_PATH) return {};
    const fs::path path = fs::path(tempPath) /
        (std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    fs::create_directories(path, error);
    return error ? fs::path() : path;
}

class Server {
public:
    ~Server() { Stop(); }

    bool Start(const fs::path& script, const fs::path& root, const fs::path& log)
    {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return false;
        if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            return false;
        }

        std::wstring command = L"python -I \"" + script.wstring() +
            L"\" --root \"" + root.wstring() + L"\" --port 0 --log \"" +
            log.wstring() + L"\"";
        std::vector<wchar_t> commandLine(command.begin(), command.end());
        commandLine.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                            &process_)) {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            return false;
        }
        CloseHandle(writePipe);
        output_ = readPipe;
        std::string portText;
        char character = 0;
        DWORD got = 0;
        while (ReadFile(output_, &character, 1, &got, nullptr) && got == 1) {
            if (character == '\n') break;
            portText.push_back(character);
            if (portText.size() > 10) break;
        }
        try {
            port_ = static_cast<INTERNET_PORT>(std::stoul(portText));
        } catch (...) {
            Stop();
            return false;
        }
        return port_ != 0;
    }

    void Stop()
    {
        if (process_.hProcess) {
            TerminateProcess(process_.hProcess, 0);
            WaitForSingleObject(process_.hProcess, 5000);
            CloseHandle(process_.hThread);
            CloseHandle(process_.hProcess);
            process_ = PROCESS_INFORMATION{};
        }
        if (output_) {
            CloseHandle(output_);
            output_ = nullptr;
        }
    }

    INTERNET_PORT port() const { return port_; }

private:
    PROCESS_INFORMATION process_{};
    HANDLE output_ = nullptr;
    INTERNET_PORT port_ = 0;
};

std::size_t CountLines(const fs::path& path)
{
    std::ifstream input(path);
    std::size_t count = 0;
    std::string line;
    while (std::getline(input, line)) ++count;
    return count;
}

bool ReadRecord(const fs::path& path, std::int64_t* hwm, int* failures)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    ob::json::Value root;
    if (ob::json::Parse(text, 32, &root) != nullptr ||
        root.type != ob::json::Type::Object) return false;
    const ob::json::Value* hwmValue = root.Find("hwm");
    const ob::json::Value* failuresValue = root.Find("failures");
    if (!hwmValue || !failuresValue || hwmValue->type != ob::json::Type::Number ||
        !failuresValue || failuresValue->type != ob::json::Type::Number) return false;
    try {
        *hwm = std::stoll(hwmValue->text);
        *failures = std::stoi(failuresValue->text);
    } catch (...) {
        return false;
    }
    return true;
}

bool RecordCurrentIsNull(const fs::path& path)
{
    const Bytes bytes = ReadBytes(path);
    const std::string text(bytes.begin(), bytes.end());
    ob::json::Value root;
    if (ob::json::Parse(text, 32, &root) != nullptr ||
        root.type != ob::json::Type::Object) return false;
    const ob::json::Value* current = root.Find("current");
    return current != nullptr && current->type == ob::json::Type::Null;
}

bool PumpUntil(const std::function<bool()>& done, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return done();
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (done()) return true;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return done();
        const DWORD remaining = static_cast<DWORD>(std::min<ULONGLONG>(
            deadline - now, 50));
        MsgWaitForMultipleObjectsEx(0, nullptr, remaining, QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
    }
}

void PumpFor(DWORD durationMs)
{
    const ULONGLONG deadline = GetTickCount64() + durationMs;
    PumpUntil([deadline]() { return GetTickCount64() >= deadline; }, durationMs + 50);
}

bool ProcessHandleCount(DWORD* count)
{
    return GetProcessHandleCount(GetCurrentProcess(), count) != FALSE;
}

bool InstallFixture(const fs::path& root, const Fixture& fixture,
                    const Bytes& webp, const Bytes& jpg)
{
    std::error_code error;
    fs::create_directories(root / "banner", error);
    if (error) return false;
    return WriteBytes(root / "banner" / "v1.signed", fixture.signedBytes) &&
           WriteBytes(root / "banner" / "hero-coffee-800.webp", webp) &&
           WriteBytes(root / "banner" / "hero-coffee-800.jpg", jpg);
}

struct LogEntry {
    std::map<std::string, std::string> headers;
};

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::vector<LogEntry> ReadLog(const fs::path& path)
{
    std::vector<LogEntry> entries;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        ob::json::Value root;
        if (ob::json::Parse(line, 32, &root) != nullptr ||
            root.type != ob::json::Type::Object) continue;
        const ob::json::Value* headers = root.Find("headers");
        if (!headers || headers->type != ob::json::Type::Object) continue;
        LogEntry entry;
        for (const auto& member : headers->members) {
            if (member.second.type == ob::json::Type::String) {
                entry.headers[Lower(member.first)] = member.second.text;
            }
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool RunMode(const fs::path& root, const fs::path& log, const KeyMaterial& key,
             INTERNET_PORT port, const char* mode, bool expectOnline)
{
    const fs::path cache = MakeTemp(L"goat-banner-cache");
    if (cache.empty()) return false;
    ob::Config config;
    config.host = L"127.0.0.1";
    config.port = port;
    config.https = false;
    config.allowInsecureForTests = true;
    config.cacheDir = cache.wstring();
    config.publicKey65 = &key.publicKey;
    config.floor = 0;
    config.kinds = {ob::Kind::Webp, ob::Kind::Jpg};
    config.userAgent = L"GOATCam-Harness/1";
    config.deadlineMs = 2000;
    config.pathPrefix = L"/" + std::wstring(mode, mode + strlen(mode));

    bool result = false;
    {
        ob::Client client(config, nullptr, WM_APP + 73);
        client.Shown();
        const fs::path record = cache / "record.json";
        if (expectOnline) {
            result = PumpUntil([&]() {
                std::int64_t hwm = 0;
                int failures = 0;
                return client.Current().online &&
                       ReadRecord(record, &hwm, &failures) && hwm == 1 && failures == 0;
            }, 7000);
        } else {
            result = PumpUntil([&]() {
                std::int64_t hwm = 0;
                int failures = 0;
                return !client.Current().online &&
                       ReadRecord(record, &hwm, &failures) && failures == 1;
            }, 7000);
        }
    }
    std::error_code error;
    fs::remove_all(cache, error);
    (void)root;
    (void)log;
    return result;
}

bool RunSlowHidden(INTERNET_PORT port, const KeyMaterial& key, const fs::path& log)
{
    const fs::path cache = MakeTemp(L"goat-banner-slow-cache");
    if (cache.empty()) return false;
    const std::size_t beforeRequests = CountLines(log);
    ob::Config config;
    config.host = L"127.0.0.1";
    config.port = port;
    config.https = false;
    config.allowInsecureForTests = true;
    config.cacheDir = cache.wstring();
    config.publicKey65 = &key.publicKey;
    config.floor = 0;
    config.kinds = {ob::Kind::Webp, ob::Kind::Jpg};
    config.userAgent = L"GOATCam-Harness/1";
    config.deadlineMs = 2000;
    config.pathPrefix = L"/slowheaders";
    {
        ob::Client client(config, nullptr, WM_APP + 73);
        client.Shown();
        PumpFor(300);
        client.Hidden();
        PumpFor(1000);
    }
    const bool noRecord = !fs::exists(cache / "record.json");
    const bool oneRequest = CountLines(log) == beforeRequests + 1;
    std::error_code error;
    fs::remove_all(cache, error);
    return noRecord && oneRequest;
}

bool RunHiddenMidAttempt(INTERNET_PORT port, const KeyMaterial& key,
                         const fs::path& log)
{
    const fs::path cache = MakeTemp(L"goat-banner-hidden-mid-attempt-cache");
    if (cache.empty()) return false;
    const std::size_t beforeRequests = CountLines(log);
    ob::Config config;
    config.host = L"127.0.0.1";
    config.port = port;
    config.https = false;
    config.allowInsecureForTests = true;
    config.cacheDir = cache.wstring();
    config.publicKey65 = &key.publicKey;
    config.floor = 0;
    config.kinds = {ob::Kind::Webp, ob::Kind::Jpg};
    config.userAgent = L"GOATCam-Harness/1";
    config.deadlineMs = 10000;
    config.pathPrefix = L"/drip";
    {
        ob::Client client(config, nullptr, WM_APP + 73);
        client.Shown();
        // The drip endpoint has sent the response headers and starts the body one byte at a
        // time. Hiding now exercises cancellation after the attempt has begun, not startup.
        PumpFor(300);
        client.Hidden();
        PumpFor(2500);
    }
    const bool noRecord = !fs::exists(cache / "record.json");
    const bool noSet = !fs::exists(cache / "sets");
    const bool oneRequest = CountLines(log) == beforeRequests + 1;
    std::error_code error;
    fs::remove_all(cache, error);
    return noRecord && noSet && oneRequest;
}

bool RunCrashSimulation(INTERNET_PORT port, const KeyMaterial& key,
                        const Fixture& revision1, const Fixture& revision2,
                        const Bytes& webp, const Bytes& jpg)
{
    const fs::path cache = MakeTemp(L"goat-banner-crash-cache");
    if (cache.empty()) return false;
    ob::Config config;
    config.host = L"127.0.0.1";
    config.port = port;
    config.https = false;
    config.allowInsecureForTests = true;
    config.cacheDir = cache.wstring();
    config.publicKey65 = &key.publicKey;
    config.floor = 0;
    config.kinds = {ob::Kind::Webp, ob::Kind::Jpg};
    config.userAgent = L"GOATCam-Harness/1";
    config.deadlineMs = 2000;
    config.pathPrefix = L"/ok";
    bool result = false;
    bool firstRun = false;
    {
        ob::Client client(config, nullptr, WM_APP + 73);
        client.Shown();
        firstRun = PumpUntil([&]() {
            std::int64_t hwm = 0;
            int failures = 0;
            return client.Current().online &&
                   ReadRecord(cache / "record.json", &hwm, &failures) &&
                   hwm == 1 && failures == 0;
        }, 7000);
    }
    const fs::path orphan = cache / "sets" / revision2.sha256;
    std::error_code error;
    fs::create_directories(orphan, error);
    const bool orphanWritten = !error &&
        WriteBytes(orphan / "v1.signed", revision2.signedBytes) &&
        WriteBytes(orphan / "photo.webp", webp) &&
        WriteBytes(orphan / "photo.jpg", jpg);
    result = firstRun && orphanWritten;
    bool secondRun = false;
    {
        ob::Client client(config, nullptr, WM_APP + 73);
        client.Shown();
        secondRun = client.Current().online &&
                    client.Current().banner.revision == revision1.revision &&
                    PumpUntil([&]() {
                     return client.Current().online &&
                            client.Current().banner.revision == revision1.revision;
                 }, 1000) && !fs::exists(orphan);
        result = result && secondRun;
    }
    fs::remove_all(cache, error);
    return result;
}

bool RunStartupTamperedExpires(const KeyMaterial& key, const Fixture& fixture,
                               const Bytes& webp, const Bytes& jpg)
{
    const fs::path cache = MakeTemp(L"goat-banner-tampered-expires-cache");
    if (cache.empty()) return false;
    const fs::path setDir = cache / "sets" / fixture.sha256;
    std::error_code error;
    fs::create_directories(setDir, error);
    if (error ||
        !WriteBytes(setDir / "v1.signed", fixture.signedBytes) ||
        !WriteBytes(setDir / "photo.webp", webp) ||
        !WriteBytes(setDir / "photo.jpg", jpg)) {
        fs::remove_all(cache, error);
        return false;
    }
    ob::json::Value banner;
    if (ob::json::Parse(fixture.json, 32, &banner) != nullptr) {
        fs::remove_all(cache, error);
        return false;
    }
    const ob::json::Value* expires = banner.Find("expires");
    std::int64_t expiresMs = 0;
    if (!expires || expires->type != ob::json::Type::String ||
        !ob::ParseTimestamp(expires->text, &expiresMs)) {
        fs::remove_all(cache, error);
        return false;
    }
    const std::string record = StoredRecord(
        fixture, webp, jpg, expiresMs + 30ll * 86400000ll);
    if (!WriteText(cache / "record.json", record)) {
        fs::remove_all(cache, error);
        return false;
    }

    ob::Config config;
    config.allowInsecureForTests = true;
    config.cacheDir = cache.wstring();
    config.publicKey65 = &key.publicKey;
    config.floor = 0;
    config.kinds = {ob::Kind::Webp, ob::Kind::Jpg};
    config.userAgent = L"GOATCam-Harness/1";
    bool result = false;
    {
        ob::Client client(config, nullptr, WM_APP + 73);
        const bool offline = !client.Current().online;
        const bool nullRecord = RecordCurrentIsNull(cache / "record.json");
        result = offline && nullRecord;
    }
    fs::remove_all(cache, error);
    return result;
}

bool RunOversizeRecord(const KeyMaterial& key)
{
    const fs::path cache = MakeTemp(L"goat-banner-oversize-record-cache");
    if (cache.empty()) return false;
    const std::size_t size = 17u * 1024u;
    const std::string oversized(size, 'x');
    const bool written = WriteText(cache / "record.json", oversized);
    ob::Config config;
    config.allowInsecureForTests = true;
    config.cacheDir = cache.wstring();
    config.publicKey65 = &key.publicKey;
    config.floor = 0;
    config.kinds = {ob::Kind::Webp, ob::Kind::Jpg};
    config.userAgent = L"GOATCam-Harness/1";
    bool result = false;
    if (written) {
        ob::Client client(config, nullptr, WM_APP + 73);
        std::error_code error;
        result = !client.Current().online &&
                 fs::exists(cache / "record.json", error) &&
                 fs::file_size(cache / "record.json", error) == size &&
                 !error;
    }
    std::error_code error;
    fs::remove_all(cache, error);
    return result;
}

bool RunStorageCaps(const KeyMaterial& key, const Fixture& fixture,
                    const Bytes& webp, const Bytes& jpg)
{
    ob::json::Value banner;
    if (ob::json::Parse(fixture.json, 32, &banner) != nullptr) return false;
    const ob::json::Value* expires = banner.Find("expires");
    std::int64_t expiresMs = 0;
    if (!expires || expires->type != ob::json::Type::String ||
        !ob::ParseTimestamp(expires->text, &expiresMs)) return false;

    const auto RunSet = [&](const wchar_t* name, const char* oversizedFile,
                            std::size_t oversizedBytes) {
        const fs::path cache = MakeTemp(name);
        if (cache.empty()) return false;
        const fs::path setDir = cache / "sets" / fixture.sha256;
        std::error_code error;
        fs::create_directories(setDir, error);
        bool prepared = !error &&
            WriteBytes(setDir / "v1.signed", fixture.signedBytes) &&
            WriteBytes(setDir / "photo.webp", webp) &&
            WriteBytes(setDir / "photo.jpg", jpg) &&
            WriteText(cache / "record.json", StoredRecord(fixture, webp, jpg, expiresMs));
        if (prepared) prepared = WriteBytes(setDir / oversizedFile,
                                            Bytes(oversizedBytes, 0x58));

        ob::Config config;
        config.allowInsecureForTests = true;
        config.cacheDir = cache.wstring();
        config.publicKey65 = &key.publicKey;
        config.floor = 0;
        config.kinds = {ob::Kind::Webp, ob::Kind::Jpg};
        config.userAgent = L"GOATCam-Harness/1";
        bool result = false;
        if (prepared) {
            ob::Client client(config, nullptr, WM_APP + 73);
            result = !client.Current().online && RecordCurrentIsNull(cache / "record.json");
        }
        fs::remove_all(cache, error);
        return result;
    };

    const bool record = [&]() {
        const fs::path cache = MakeTemp(L"goat-banner-cap-record-cache");
        if (cache.empty()) return false;
        const std::size_t size = 16u * 1024u + 1u;
        const bool written = WriteBytes(cache / "record.json", Bytes(size, 0x58));
        ob::Config config;
        config.allowInsecureForTests = true;
        config.cacheDir = cache.wstring();
        config.publicKey65 = &key.publicKey;
        config.floor = 0;
        config.kinds = {ob::Kind::Webp, ob::Kind::Jpg};
        config.userAgent = L"GOATCam-Harness/1";
        bool result = false;
        if (written) {
            ob::Client client(config, nullptr, WM_APP + 73);
            std::error_code error;
            result = !client.Current().online && fs::exists(cache / "record.json", error) &&
                     fs::file_size(cache / "record.json", error) == size && !error;
        }
        std::error_code error;
        fs::remove_all(cache, error);
        return result;
    }();
    const bool signedCap = RunSet(L"goat-banner-cap-signed-cache", "v1.signed",
                                  16384u + 1u);
    const bool photoCap = RunSet(L"goat-banner-cap-photo-cache", "photo.webp",
                                 300000u + 1u);
    return record && signedCap && photoCap;
}

bool RunAtomicSet(INTERNET_PORT port, const KeyMaterial& key,
                  const Fixture& revision1, const Fixture& revision2,
                  const Bytes& webp, const Bytes& jpg)
{
    const fs::path cache = MakeTemp(L"goat-banner-atomic-set-cache");
    if (cache.empty()) return false;
    ob::Config config;
    config.host = L"127.0.0.1";
    config.port = port;
    config.https = false;
    config.allowInsecureForTests = true;
    config.cacheDir = cache.wstring();
    config.publicKey65 = &key.publicKey;
    config.floor = 0;
    config.kinds = {ob::Kind::Webp, ob::Kind::Jpg};
    config.userAgent = L"GOATCam-Harness/1";
    config.deadlineMs = 2000;
    config.pathPrefix = L"/ok";

    bool firstRun = false;
    {
        ob::Client client(config, nullptr, WM_APP + 73);
        client.Shown();
        firstRun = PumpUntil([&]() {
            std::int64_t hwm = 0;
            int failures = 0;
            return client.Current().online &&
                   ReadRecord(cache / "record.json", &hwm, &failures) &&
                   hwm == revision1.revision && failures == 0;
        }, 7000);
    }

    // A terminated writer is represented by a new set containing only the first two
    // files. Startup must keep the old complete set and remove the incomplete one.
    const fs::path partial = cache / "sets" / revision2.sha256;
    std::error_code error;
    fs::create_directories(partial, error);
    const bool partialWritten = !error &&
        WriteBytes(partial / "v1.signed", revision2.signedBytes) &&
        WriteBytes(partial / "photo.webp", webp) &&
        !fs::exists(partial / "photo.jpg");

    bool secondRun = false;
    bool oldSetIntact = false;
    {
        ob::Client client(config, nullptr, WM_APP + 73);
        client.Shown();
        oldSetIntact = fs::exists(cache / "sets" / revision1.sha256 / "v1.signed") &&
                       fs::exists(cache / "sets" / revision1.sha256 / "photo.jpg");
        secondRun = client.Current().online &&
                    client.Current().banner.revision == revision1.revision &&
                    PumpUntil([&]() {
            return client.Current().online &&
                   client.Current().banner.revision == revision1.revision;
                    }, 1000) && !fs::exists(partial);
    }
    const bool result = firstRun && partialWritten && oldSetIntact && secondRun;
    fs::remove_all(cache, error);
    return result;
}

bool RunPoisonedRecord(INTERNET_PORT port, const KeyMaterial& key)
{
    const fs::path cache = MakeTemp(L"goat-banner-poisoned-record-cache");
    if (cache.empty()) return false;
    const bool written = WriteText(
        cache / "record.json",
        "{\"hwm\":9007199254740992,\"current\":null,\"lastCheckMs\":0,\"failures\":0}");
    ob::Config config;
    config.host = L"127.0.0.1";
    config.port = port;
    config.https = false;
    config.allowInsecureForTests = true;
    config.cacheDir = cache.wstring();
    config.publicKey65 = &key.publicKey;
    config.floor = 0;
    config.kinds = {ob::Kind::Webp, ob::Kind::Jpg};
    config.userAgent = L"GOATCam-Harness/1";
    config.deadlineMs = 2000;
    config.pathPrefix = L"/ok";
    bool result = false;
    if (written) {
        ob::Client client(config, nullptr, WM_APP + 73);
        client.Shown();
        result = PumpUntil([&]() {
            std::int64_t hwm = 0;
            int failures = 0;
            return client.Current().online &&
                   ReadRecord(cache / "record.json", &hwm, &failures) &&
                   hwm == 1 && failures == 0;
        }, 7000);
    }
    std::error_code error;
    fs::remove_all(cache, error);
    return result;
}

bool RunWorkerReaping(INTERNET_PORT port, const KeyMaterial& key)
{
    const fs::path cache = MakeTemp(L"goat-banner-worker-cache");
    if (cache.empty()) return false;
    ob::Config config;
    config.host = L"127.0.0.1";
    config.port = port;
    config.https = false;
    config.allowInsecureForTests = true;
    config.cacheDir = cache.wstring();
    config.publicKey65 = &key.publicKey;
    config.floor = 0;
    config.kinds = {ob::Kind::Webp, ob::Kind::Jpg};
    config.userAgent = L"GOATCam-Harness/1";
    config.deadlineMs = 2000;
    config.pathPrefix = L"/ok";

    bool result = true;
    {
        ob::Client client(config, nullptr, WM_APP + 73);
        DWORD baseline = 0;
        result = ProcessHandleCount(&baseline);
        for (int cycle = 0; cycle < 20 && result; ++cycle) {
            client.Shown();
            client.Hidden();
            PumpFor(25);
            DWORD current = 0;
            result = ProcessHandleCount(&current) && current < baseline + 10;
        }
    }
    std::error_code error;
    fs::remove_all(cache, error);
    return result;
}

bool CheckHeaders(const fs::path& log)
{
    const std::vector<LogEntry> entries = ReadLog(log);
    std::set<std::string> names;
    bool good = !entries.empty();
    for (const LogEntry& entry : entries) {
        for (const auto& header : entry.headers) names.insert(header.first);
        const auto userAgent = entry.headers.find("user-agent");
        if (userAgent == entry.headers.end() || userAgent->second != "GOATCam-Harness/1") {
            good = false;
        }
        for (const char* forbidden : {"cookie", "referer", "accept-encoding", "authorization"}) {
            if (entry.headers.find(forbidden) != entry.headers.end()) good = false;
        }
    }
    std::cout << "HEADER NAMES SEEN:";
    for (const std::string& name : names) std::cout << ' ' << name;
    std::cout << '\n';
    return good;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 1) return 1;
    const fs::path executable = fs::absolute(fs::path(argv[0]));
    const fs::path repo = executable.parent_path().parent_path();
    const fs::path assetDir = repo / "tests" / "banner-vectors" / "photos";
    const fs::path script = repo / "tests" / "banner-testserver" / "serve.py";
    const Bytes webp = ReadBytes(assetDir / "hero-coffee-800.webp");
    const Bytes jpg = ReadBytes(assetDir / "hero-coffee-800.jpg");
    KeyMaterial key;
    int failures = 0;
    if (webp.empty() || jpg.empty() || !MakeKey(&key)) {
        std::cout << "BANNER CLIENT HARNESS: FAIL=1\n";
        return 1;
    }

    const Fixture revision1 = MakeFixture(&key, webp, jpg, 1);
    const Fixture revision2 = MakeFixture(&key, webp, jpg, 2);
    const fs::path serverRoot = MakeTemp(L"goat-banner-server");
    if (serverRoot.empty() || !InstallFixture(serverRoot, revision1, webp, jpg)) {
        std::cout << "BANNER CLIENT HARNESS: FAIL=1\n";
        return 1;
    }
    const fs::path log = serverRoot / "requests.log";
    Server server;
    if (!server.Start(script, serverRoot, log)) {
        std::cout << "BANNER CLIENT HARNESS: FAIL=1\n";
        return 1;
    }

    const std::vector<std::pair<const char*, bool>> cases = {
        {"ok", true}, {"drip", false}, {"redirect", false},
        {"redirect-cross", false}, {"gzip", false}, {"bigheaders", false},
        {"status500", false},
    };
    for (const auto& test : cases) {
        const bool passed = RunMode(serverRoot, log, key, server.port(),
                                    test.first, test.second);
        std::cout << test.first << ": " << (passed ? "PASS" : "FAIL") << '\n';
        if (!passed) ++failures;
    }
    const bool slowPassed = RunSlowHidden(server.port(), key, log);
    std::cout << "slowheaders: " << (slowPassed ? "PASS" : "FAIL") << '\n';
    if (!slowPassed) ++failures;

    const bool hiddenMidAttemptPassed = RunHiddenMidAttempt(server.port(), key, log);
    std::cout << "AO3 hidden-mid-attempt: "
              << (hiddenMidAttemptPassed ? "PASS" : "FAIL") << '\n';
    if (!hiddenMidAttemptPassed) ++failures;

    const bool crashPassed = RunCrashSimulation(server.port(), key, revision1,
                                                revision2, webp, jpg);
    std::cout << "crash-simulation: " << (crashPassed ? "PASS" : "FAIL") << '\n';
    if (!crashPassed) ++failures;

    const bool tamperedExpiresPassed = RunStartupTamperedExpires(
        key, revision1, webp, jpg);
    std::cout << "startup-tampered-expires: "
              << (tamperedExpiresPassed ? "PASS" : "FAIL") << '\n';
    if (!tamperedExpiresPassed) ++failures;

    const bool oversizeRecordPassed = RunOversizeRecord(key);
    std::cout << "oversize-record: "
              << (oversizeRecordPassed ? "PASS" : "FAIL") << '\n';
    if (!oversizeRecordPassed) ++failures;

    const bool storageCapsPassed = RunStorageCaps(key, revision1, webp, jpg);
    std::cout << "AO22 storage-caps: "
              << (storageCapsPassed ? "PASS" : "FAIL") << '\n';
    if (!storageCapsPassed) ++failures;

    const bool atomicSetPassed = RunAtomicSet(server.port(), key, revision1,
                                              revision2, webp, jpg);
    std::cout << "AO22 atomic-set: "
              << (atomicSetPassed ? "PASS" : "FAIL") << '\n';
    if (!atomicSetPassed) ++failures;

    const bool poisonedRecordPassed = RunPoisonedRecord(server.port(), key);
    std::cout << "poisoned-record: "
              << (poisonedRecordPassed ? "PASS" : "FAIL") << '\n';
    if (!poisonedRecordPassed) ++failures;

    const bool workerReapingPassed = RunWorkerReaping(server.port(), key);
    std::cout << "worker-reaping: "
              << (workerReapingPassed ? "PASS" : "FAIL") << '\n';
    if (!workerReapingPassed) ++failures;

    const bool headersPassed = CheckHeaders(log);
    std::cout << "headers: " << (headersPassed ? "PASS" : "FAIL") << '\n';
    if (!headersPassed) ++failures;

    server.Stop();
    std::error_code error;
    fs::remove_all(serverRoot, error);
    std::cout << "BANNER CLIENT HARNESS: FAIL=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
