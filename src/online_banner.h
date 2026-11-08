#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ob {

enum class Action { Show, KillSwitch, Reject };

struct Banner {
    std::wstring headline{};
    std::wstring headlineEm{};
    std::wstring wordmark{};
    std::wstring badge{};
    std::wstring labelLink{};
    std::wstring labelQr{};
    std::vector<std::wstring> tagline{};
    std::string link{};
    std::string qrText{};
    std::string photoWebp{};
    std::string photoJpg{};
    std::string sha256Webp{};
    std::string sha256Jpg{};
    std::vector<std::string> qrRows{};
    double focus[2]{};
    double textBox[4]{};
    double steamAt[2]{};
    int w = 0;
    int h = 0;
    std::int64_t revision = 0;
    std::int64_t issuedMs = 0;
    std::int64_t expiresMs = 0;
    bool enabled = false;
    bool viewfinder = false;
    bool steam = false;
    bool hasHeadlineEm = false;
    bool hasBadge = false;
};

struct Context {
    const std::uint8_t (*publicKey65)[65] = nullptr;
    std::int64_t nowMs = 0;
    std::int64_t hwm = 0;
    std::int64_t floor = 0;
    const std::string* storedSigned = nullptr;
};

struct Result {
    Action action = Action::Reject;
    const char* reason = nullptr;
    Banner banner{};
    std::string sha256{};
};

Result Evaluate(const std::string& signedBytes, const Context& ctx);
const char* CheckPhoto(const std::string& bytes, bool jpg,
                       const std::string& sha256Hex, int w, int h);
bool ParseTimestamp(const std::string& text, std::int64_t* outMs);
std::string KeyIdOf(const std::uint8_t (&publicKey65)[65]);
bool VerifyP256(const std::uint8_t (&publicKey65)[65],
               const std::string& message, const std::uint8_t sig64[64]);

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool b = false;
    std::string text{};
    std::vector<Value> items{};
    std::vector<std::pair<std::string, Value>> members{};

    const Value* Find(const char* key) const;
};

const char* Parse(const std::string& utf8, int maxDepth, Value* out,
                  int* observedDepth = nullptr);

} // namespace json
} // namespace ob
