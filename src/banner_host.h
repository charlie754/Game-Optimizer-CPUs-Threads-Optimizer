#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "online_banner_client.h"
#include "sponsor.h"

#include <string>

namespace cd {

// The message sent to a subscriber after the process-wide client changes its model.
constexpr UINT kBannerHostChanged = WM_APP + 0x4d;

int BannerHostSubscribe(HWND target, UINT msg);
void BannerHostUnsubscribe(int token);
void BannerHostVisible(int token, bool visible);
void BannerHostPointer(bool over);
void BannerHostFocus(bool inside);
bool BannerHostClick(std::string* url);
void BannerHostResumed();
ob::Model BannerHostCurrent();

// Serializes the shared model into the exact object consumed by the WebView2 page.
std::wstring PageMessageJson(const ob::Model& model);

// The WebView2 and GDI paths share this exact allow-list. The current signed link is allowed
// only when `current.online` is true; a stale/offline model cannot authorize a navigation.
bool SponsorLinkAllowed(const std::wstring& uri, const ob::Model& current);

#if defined(CD_TESTS) || defined(CD_SPONSOR_HARNESS)
// Test/capture seam. The pointer is borrowed until the next call; nullptr restores the real
// client. It intentionally does not exist in the production header ABI.
void BannerHostSetModelForTest(const ob::Model* model);
#endif

}  // namespace cd
