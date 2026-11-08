#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "online_banner_machine.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ob {

struct Config {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool https = true;
    std::wstring cacheDir;
    const std::uint8_t (*publicKey65)[65] = nullptr;
    bool allowInsecureForTests = false;
    std::int64_t floor = 0;
    std::vector<Kind> kinds;
    std::wstring userAgent;
    DWORD deadlineMs = 20000;
    std::wstring pathPrefix;
};

struct Model {
    bool online = false;
    Banner banner;
    std::string webp;
    std::string jpg;
};

class Client {
public:
    Client(Config config, HWND notify, UINT notifyMsg);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void Shown();
    void Hidden();
    void Pointer(bool over);
    void Focus(bool inside);
    bool Click(std::string* urlToOpen);
    void TimerFired();
    void Resumed();
    Model Current() const;
    UINT_PTR TimerId() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ob
