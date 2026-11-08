#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include "online_banner.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#pragma comment(lib, "bcrypt.lib")

namespace ob {
namespace {

constexpr std::size_t kMaxSignedBytes = 16384u;
constexpr std::size_t kMaxPhotoBytes = 300000u;
constexpr std::int64_t kDayMs = 86400000ll;
constexpr std::int64_t kMaxRevision = 9007199254740991ll;
constexpr char kSigPrefix[] = "GOATCAM-BANNER-V1\n";

bool IsDigit(char ch)
{
    return ch >= '0' && ch <= '9';
}

int HexValue(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

bool ReadUtf8CodePoint(const std::string& text, std::size_t* pos,
                       std::uint32_t* codePoint)
{
    if (*pos >= text.size()) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(text[*pos]);
    if (first < 0x80u) {
        ++*pos;
        *codePoint = first;
        return true;
    }

    int count = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xC2u && first <= 0xDFu) {
        count = 1;
        value = first & 0x1Fu;
        minimum = 0x80u;
    } else if (first >= 0xE0u && first <= 0xEFu) {
        count = 2;
        value = first & 0x0Fu;
        minimum = 0x800u;
    } else if (first >= 0xF0u && first <= 0xF4u) {
        count = 3;
        value = first & 0x07u;
        minimum = 0x10000u;
    } else {
        return false;
    }
    if (text.size() - *pos <= static_cast<std::size_t>(count)) {
        return false;
    }
    for (int i = 1; i <= count; ++i) {
        const unsigned char next = static_cast<unsigned char>(text[*pos + i]);
        if ((next & 0xC0u) != 0x80u) {
            return false;
        }
        value = (value << 6) | (next & 0x3Fu);
    }
    if (value < minimum || value > 0x10FFFFu ||
        (value >= 0xD800u && value <= 0xDFFFu)) {
        return false;
    }
    *pos += static_cast<std::size_t>(count) + 1u;
    *codePoint = value;
    return true;
}

void AppendUtf8(std::string* out, std::uint32_t codePoint)
{
    if (codePoint < 0x80u) {
        out->push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800u) {
        out->push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
        out->push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else if (codePoint < 0x10000u) {
        out->push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
        out->push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else {
        out->push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
        out->push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    }
}

bool Hex4(const std::string& text, std::size_t* pos, std::uint32_t* value)
{
    if (text.size() - *pos < 4u) {
        return false;
    }
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
        const int digit = HexValue(text[*pos + static_cast<std::size_t>(i)]);
        if (digit < 0) {
            return false;
        }
        result = (result << 4) | static_cast<std::uint32_t>(digit);
    }
    *pos += 4u;
    *value = result;
    return true;
}

bool DecodeUtf8(const std::string& text, std::vector<std::uint32_t>* codePoints,
                std::wstring* wide)
{
    if (codePoints != nullptr) {
        codePoints->clear();
    }
    if (wide != nullptr) {
        wide->clear();
    }
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::uint32_t codePoint = 0;
        if (!ReadUtf8CodePoint(text, &pos, &codePoint)) {
            return false;
        }
        if (codePoints != nullptr) {
            codePoints->push_back(codePoint);
        }
        if (wide != nullptr) {
            if (codePoint <= 0xFFFFu) {
                wide->push_back(static_cast<wchar_t>(codePoint));
            } else {
                const std::uint32_t value = codePoint - 0x10000u;
                wide->push_back(static_cast<wchar_t>(0xD800u + (value >> 10)));
                wide->push_back(static_cast<wchar_t>(0xDC00u + (value & 0x3FFu)));
            }
        }
    }
    return true;
}

} // namespace

namespace json {
namespace {

struct Parser {
    const std::string& input;
    std::size_t pos = 0;
    int maxDepth = 0;
    int observedDepth = 0;
    bool duplicate = false;
    bool depth = false;
    bool syntax = false;

    void SkipWhitespace()
    {
        while (pos < input.size() &&
               (input[pos] == ' ' || input[pos] == '\t' ||
                input[pos] == '\r' || input[pos] == '\n')) {
            ++pos;
        }
    }

    bool Eat(char ch)
    {
        if (pos < input.size() && input[pos] == ch) {
            ++pos;
            return true;
        }
        return false;
    }

    bool String(std::string* out)
    {
        if (!Eat('"')) {
            syntax = true;
            return false;
        }
        out->clear();
        while (pos < input.size()) {
            const unsigned char ch = static_cast<unsigned char>(input[pos++]);
            if (ch == '"') {
                return true;
            }
            if (ch < 0x20u) {
                syntax = true;
                return false;
            }
            if (ch != '\\') {
                if (ch < 0x80u) {
                    out->push_back(static_cast<char>(ch));
                } else {
                    --pos;
                    const std::size_t start = pos;
                    std::uint32_t codePoint = 0;
                    if (!ReadUtf8CodePoint(input, &pos, &codePoint)) {
                        syntax = true;
                        return false;
                    }
                    out->append(input, start, pos - start);
                }
                continue;
            }
            if (pos >= input.size()) {
                syntax = true;
                return false;
            }
            const char escaped = input[pos++];
            switch (escaped) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                std::uint32_t codePoint = 0;
                if (!Hex4(input, &pos, &codePoint)) {
                    syntax = true;
                    return false;
                }
                if (codePoint >= 0xD800u && codePoint <= 0xDBFFu) {
                    if (input.size() - pos < 6u || input[pos] != '\\' ||
                        input[pos + 1u] != 'u') {
                        syntax = true;
                        return false;
                    }
                    pos += 2u;
                    std::uint32_t low = 0;
                    if (!Hex4(input, &pos, &low) || low < 0xDC00u ||
                        low > 0xDFFFu) {
                        syntax = true;
                        return false;
                    }
                    codePoint = 0x10000u + ((codePoint - 0xD800u) << 10) +
                                (low - 0xDC00u);
                } else if (codePoint >= 0xDC00u && codePoint <= 0xDFFFu) {
                    syntax = true;
                    return false;
                }
                AppendUtf8(out, codePoint);
                break;
            }
            default:
                syntax = true;
                return false;
            }
        }
        syntax = true;
        return false;
    }

    bool Number(Value* out)
    {
        const std::size_t start = pos;
        Eat('-');
        if (Eat('0')) {
            // A leading zero cannot be followed by another integer digit.
        } else if (pos < input.size() && input[pos] >= '1' && input[pos] <= '9') {
            while (pos < input.size() && IsDigit(input[pos])) {
                ++pos;
            }
        } else {
            syntax = true;
            return false;
        }
        if (Eat('.')) {
            if (pos >= input.size() || !IsDigit(input[pos])) {
                syntax = true;
                return false;
            }
            while (pos < input.size() && IsDigit(input[pos])) {
                ++pos;
            }
        }
        if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
            ++pos;
            if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) {
                ++pos;
            }
            if (pos >= input.size() || !IsDigit(input[pos])) {
                syntax = true;
                return false;
            }
            while (pos < input.size() && IsDigit(input[pos])) {
                ++pos;
            }
        }
        out->type = Type::Number;
        out->text.assign(input, start, pos - start);
        return true;
    }

    bool Literal(const char* word, Value* out, Type type, bool boolean)
    {
        const std::size_t length = std::strlen(word);
        if (input.size() - pos < length || input.compare(pos, length, word) != 0) {
            syntax = true;
            return false;
        }
        pos += length;
        out->type = type;
        out->b = boolean;
        return true;
    }

    bool ValueAt(int containerDepth, Value* out)
    {
        SkipWhitespace();
        if (pos >= input.size()) {
            syntax = true;
            return false;
        }
        switch (input[pos]) {
        case '{': return Object(containerDepth, out);
        case '[': return Array(containerDepth, out);
        case '"':
            out->type = Type::String;
            return String(&out->text);
        case 't': return Literal("true", out, Type::Bool, true);
        case 'f': return Literal("false", out, Type::Bool, false);
        case 'n': return Literal("null", out, Type::Null, false);
        default: return Number(out);
        }
    }

    bool Object(int depthValue, Value* out)
    {
        if (depthValue > observedDepth) {
            observedDepth = depthValue;
        }
        if (depthValue >= maxDepth) {
            depth = true;
            return false;
        }
        ++pos;
        out->type = Type::Object;
        out->members.clear();
        SkipWhitespace();
        if (Eat('}')) {
            return true;
        }
        for (;;) {
            SkipWhitespace();
            std::string key;
            if (!String(&key)) {
                return false;
            }
            // ponytail: O(k^2) duplicate scan is bounded by the 16 KiB file cap; upgrade to a hash set if that cap or object member counts grow.
            for (const auto& member : out->members) {
                if (member.first == key) {
                    duplicate = true;
                    break;
                }
            }
            SkipWhitespace();
            if (!Eat(':')) {
                syntax = true;
                return false;
            }
            Value value;
            if (!ValueAt(depthValue + 1, &value)) {
                return false;
            }
            out->members.emplace_back(std::move(key), std::move(value));
            SkipWhitespace();
            if (Eat('}')) {
                return true;
            }
            if (!Eat(',')) {
                syntax = true;
                return false;
            }
        }
    }

    bool Array(int depthValue, Value* out)
    {
        if (depthValue > observedDepth) {
            observedDepth = depthValue;
        }
        if (depthValue >= maxDepth) {
            depth = true;
            return false;
        }
        ++pos;
        out->type = Type::Array;
        out->items.clear();
        SkipWhitespace();
        if (Eat(']')) {
            return true;
        }
        for (;;) {
            Value value;
            if (!ValueAt(depthValue + 1, &value)) {
                return false;
            }
            out->items.push_back(std::move(value));
            SkipWhitespace();
            if (Eat(']')) {
                return true;
            }
            if (!Eat(',')) {
                syntax = true;
                return false;
            }
        }
    }
};

} // namespace

const Value* Value::Find(const char* key) const
{
    if (type != Type::Object || key == nullptr) {
        return nullptr;
    }
    for (const auto& member : members) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

const char* Parse(const std::string& utf8, int maxDepth, Value* out,
                  int* observedDepth)
{
    if (observedDepth != nullptr) {
        *observedDepth = 0;
    }
    if (out == nullptr || maxDepth < 1) {
        return "json";
    }
    try {
        *out = Value{};
        const int cappedDepth = maxDepth > 64 ? 64 : maxDepth;
        Parser parser{utf8, 0, cappedDepth, 0, false, false, false};
        auto Finish = [&parser, observedDepth](const char* reason) {
            if (observedDepth != nullptr) {
                *observedDepth = parser.observedDepth;
            }
            return reason;
        };
        if (!parser.ValueAt(1, out)) {
            if (parser.depth) {
                return Finish("depth");
            }
            if (parser.syntax) {
                return Finish("json");
            }
            if (parser.duplicate) {
                return Finish("dupkey");
            }
            return Finish("json");
        }
        parser.SkipWhitespace();
        if (parser.pos != utf8.size()) {
            parser.syntax = true;
        }
        if (parser.syntax) {
            return Finish("json");
        }
        if (parser.duplicate) {
            return Finish("dupkey");
        }
        return Finish(nullptr);
    } catch (...) {
        return "json";
    }
}

} // namespace json

namespace {

struct KeyHandle {
    BCRYPT_KEY_HANDLE handle = nullptr;
    ~KeyHandle()
    {
        if (handle != nullptr) {
            BCryptDestroyKey(handle);
        }
    }
};

struct AlgHandle {
    BCRYPT_ALG_HANDLE handle = nullptr;
    ~AlgHandle()
    {
        if (handle != nullptr) {
            BCryptCloseAlgorithmProvider(handle, 0);
        }
    }
};

bool Sha256(const std::uint8_t* data, std::size_t length,
            std::uint8_t output[32])
{
    if ((data == nullptr && length != 0u) ||
        length > static_cast<std::size_t>(std::numeric_limits<ULONG>::max())) {
        return false;
    }
    return BCRYPT_SUCCESS(BCryptHash(
        BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data),
        static_cast<ULONG>(length), output, 32));
}

bool ImportP256(const std::uint8_t publicKeyXY[64], AlgHandle* algorithm,
               KeyHandle* key)
{
    // ponytail: ceiling is one BCrypt provider open per VerifyP256 call; upgrade to a synchronized provider cache if verification throughput matters.
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm->handle, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
        return false;
    }
    std::uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64u]{};
    BCRYPT_ECCKEY_BLOB header{};
    header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header.cbKey = 32;
    std::memcpy(blob, &header, sizeof(header));
    std::memcpy(blob + sizeof(header), publicKeyXY, 64u);
    return BCRYPT_SUCCESS(BCryptImportKeyPair(
        algorithm->handle, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key->handle, blob,
        static_cast<ULONG>(sizeof(blob)), 0));
}

std::string Hex(const std::uint8_t* bytes, std::size_t length)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2u);
    for (std::size_t i = 0; i < length; ++i) {
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 0x0Fu]);
    }
    return result;
}

bool IsLowerHex64(const std::string& text)
{
    if (text.size() != 64u) {
        return false;
    }
    for (const char ch : text) {
        if (!(IsDigit(ch) || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool VerifyRaw(const std::uint8_t (&publicKey65)[65], const std::string& message,
               const std::uint8_t sig64[64])
{
    if (sig64 == nullptr || publicKey65[0] != 0x04u) {
        return false;
    }
    std::uint8_t digest[32]{};
    const std::uint8_t* messageBytes = message.empty()
                                           ? nullptr
                                           : reinterpret_cast<const std::uint8_t*>(message.data());
    if (!Sha256(messageBytes, message.size(), digest)) {
        return false;
    }
    AlgHandle algorithm;
    KeyHandle key;
    if (!ImportP256(publicKey65 + 1u, &algorithm, &key)) {
        return false;
    }
    return BCRYPT_SUCCESS(BCryptVerifySignature(
        key.handle, nullptr, digest, 32, const_cast<PUCHAR>(sig64), 64, 0));
}

bool ParseUnsignedInteger(const json::Value* value, std::uint64_t* output)
{
    if (value == nullptr || value->type != json::Type::Number ||
        value->text.empty()) {
        return false;
    }
    std::uint64_t result = 0;
    for (const char ch : value->text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
            return false;
        }
        result = result * 10u + digit;
    }
    *output = result;
    return true;
}

bool ParseInt64(const json::Value* value, std::int64_t* output)
{
    std::uint64_t unsignedValue = 0;
    if (!ParseUnsignedInteger(value, &unsignedValue) ||
        unsignedValue > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    *output = static_cast<std::int64_t>(unsignedValue);
    return true;
}

bool IsIntegerToken(const std::string& text)
{
    if (text.empty()) {
        return false;
    }
    const std::size_t first = text[0] == '-' ? 1u : 0u;
    if (first == text.size()) {
        return false;
    }
    for (std::size_t i = first; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

const json::Value* Field(const json::Value& object, const char* name)
{
    return object.Find(name);
}

bool IsNumberInUnitRange(const json::Value* value, double* output)
{
    if (value == nullptr || value->type != json::Type::Number) {
        return false;
    }
    double parsed = 0.0;
    const char* first = value->text.data();
    const char* last = first + value->text.size();
    const auto conversion = std::from_chars(first, last, parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != last ||
        !std::isfinite(parsed) ||
        parsed < 0.0 || parsed > 1.0) {
        return false;
    }
    *output = parsed;
    return true;
}

bool ParseArrayNumbers(const json::Value* value, std::size_t count,
                       double* output)
{
    if (value == nullptr || value->type != json::Type::Array ||
        value->items.size() != count) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!IsNumberInUnitRange(&value->items[i], &output[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

bool ParseTimestamp(const std::string& text, std::int64_t* output)
{
    if (text.size() != 20u || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
        text[19] != 'Z') {
        return false;
    }
    const std::size_t digitPositions[] = {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u,
                                          11u, 12u, 14u, 15u, 17u, 18u};
    for (const std::size_t pos : digitPositions) {
        if (!IsDigit(text[pos])) {
            return false;
        }
    }
    auto NumberAt = [&text](std::size_t start, std::size_t count) {
        int result = 0;
        for (std::size_t i = 0; i < count; ++i) {
            result = result * 10 + (text[start + i] - '0');
        }
        return result;
    };
    const int year = NumberAt(0u, 4u);
    const unsigned month = static_cast<unsigned>(NumberAt(5u, 2u));
    const unsigned day = static_cast<unsigned>(NumberAt(8u, 2u));
    const unsigned hour = static_cast<unsigned>(NumberAt(11u, 2u));
    const unsigned minute = static_cast<unsigned>(NumberAt(14u, 2u));
    const unsigned second = static_cast<unsigned>(NumberAt(17u, 2u));
    if (year < 2000 || year > 9999 || month < 1u || month > 12u ||
        hour > 23u || minute > 59u || second > 59u) {
        return false;
    }
    const unsigned leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 1u : 0u;
    const unsigned daysInMonth[] = {0u, 31u, 28u + leap, 31u, 30u, 31u, 30u,
                                    31u, 31u, 30u, 31u, 30u, 31u};
    if (day < 1u || day > daysInMonth[month]) {
        return false;
    }
    int adjustedYear = year - (month <= 2u ? 1 : 0);
    const int era = (adjustedYear >= 0 ? adjustedYear : adjustedYear - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(adjustedYear - era * 400);
    const unsigned monthPrime = month > 2u ? month - 3u : month + 9u;
    const unsigned dayOfYear = (153u * monthPrime + 2u) / 5u + day - 1u;
    const unsigned dayOfEra = yearOfEra * 365u + yearOfEra / 4u -
                              yearOfEra / 100u + dayOfYear;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097ll +
                              static_cast<std::int64_t>(dayOfEra) - 719468ll;
    *output = days * kDayMs + static_cast<std::int64_t>(hour) * 3600000ll +
              static_cast<std::int64_t>(minute) * 60000ll +
              static_cast<std::int64_t>(second) * 1000ll;
    return true;
}

namespace {

bool AfterPlusDay(std::int64_t value, std::int64_t base, std::int64_t amount)
{
    if (base > std::numeric_limits<std::int64_t>::max() - amount) {
        return false;
    }
    return value > base + amount;
}

bool AllowedTextCodePoint(std::uint32_t codePoint)
{
    return (codePoint >= 0x20u && codePoint <= 0x7Eu) ||
           (codePoint >= 0xA0u && codePoint <= 0xACu) ||
           (codePoint >= 0xAEu && codePoint <= 0x24Fu) ||
           (codePoint >= 0x2013u && codePoint <= 0x2014u) ||
           (codePoint >= 0x2018u && codePoint <= 0x2019u) ||
           (codePoint >= 0x201Cu && codePoint <= 0x201Du) ||
           codePoint == 0x2026u || codePoint == 0x2122u;
}

bool CheckText(const json::Value* value, std::size_t minimum,
               std::size_t maximum, std::wstring* wide)
{
    if (value == nullptr || value->type != json::Type::String) {
        return false;
    }
    std::vector<std::uint32_t> codePoints;
    if (!DecodeUtf8(value->text, &codePoints, wide) ||
        codePoints.size() < minimum || codePoints.size() > maximum) {
        return false;
    }
    for (const std::uint32_t codePoint : codePoints) {
        if (!AllowedTextCodePoint(codePoint)) {
            return false;
        }
    }
    return true;
}

bool PhotoName(const std::string& value, const char* suffix)
{
    const std::size_t suffixLength = std::strlen(suffix);
    if (value.size() <= suffixLength ||
        value.size() - suffixLength > 64u ||
        value.compare(value.size() - suffixLength, suffixLength, suffix) != 0) {
        return false;
    }
    for (std::size_t i = 0; i < value.size() - suffixLength; ++i) {
        const char ch = value[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) {
            return false;
        }
    }
    return true;
}

bool Url(const std::string& value)
{
    if (value.empty() || value.size() > 200u || value.compare(0, 8u, "https://") != 0) {
        return false;
    }
    for (const unsigned char ch : value) {
        if (ch < 0x21u || ch > 0x7Eu) {
            return false;
        }
    }
    const std::size_t authorityEnd = value.find_first_of("/?#", 8u);
    const std::string authority = value.substr(8u, authorityEnd - 8u);
    if (authority.empty()) {
        return false;
    }
    std::string host = authority;
    const std::size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        if (authority.compare(colon, authority.size() - colon, ":443") != 0) {
            return false;
        }
        host = authority.substr(0, colon);
    }
    if (host.size() > 253u || host.empty() || host.front() == '.' || host.back() == '.') {
        return false;
    }
    std::size_t labelStart = 0;
    std::size_t labelCount = 0;
    std::string lastLabel;
    for (std::size_t i = 0; i <= host.size(); ++i) {
        if (i != host.size() && host[i] != '.') {
            continue;
        }
        const std::size_t labelLength = i - labelStart;
        if (labelLength == 0u || labelLength > 63u) {
            return false;
        }
        const std::string label = host.substr(labelStart, labelLength);
        if (label.front() == '-' || label.back() == '-') {
            return false;
        }
        for (const char ch : label) {
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '-')) {
                return false;
            }
        }
        ++labelCount;
        lastLabel = label;
        labelStart = i + 1u;
    }
    if (labelCount < 2u) {
        return false;
    }
    bool asciiTld = lastLabel.size() >= 2u && lastLabel.size() <= 63u;
    for (const char ch : lastLabel) {
        if (!(ch >= 'A' && ch <= 'Z') && !(ch >= 'a' && ch <= 'z')) {
            asciiTld = false;
            break;
        }
    }
    bool punycodeTld = lastLabel.size() >= 5u && lastLabel.size() <= 63u &&
                       lastLabel.compare(0, 4u, "xn--") == 0;
    if (punycodeTld) {
        for (std::size_t i = 4u; i < lastLabel.size(); ++i) {
            const char ch = lastLabel[i];
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '-')) {
                punycodeTld = false;
                break;
            }
        }
    }
    return asciiTld || punycodeTld;
}

bool Base64Line(const std::string& text, std::vector<std::uint8_t>* output)
{
    if (text.size() != 88u) {
        return false;
    }
    auto Value = [](char ch) {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };
    output->clear();
    output->reserve(64u);
    for (std::size_t i = 0; i < text.size(); i += 4u) {
        const int a = Value(text[i]);
        const int b = Value(text[i + 1u]);
        const bool last = i + 4u == text.size();
        const int c = text[i + 2u] == '=' ? 0 : Value(text[i + 2u]);
        const int d = text[i + 3u] == '=' ? 0 : Value(text[i + 3u]);
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (!last && (text[i + 2u] == '=' || text[i + 3u] == '=')) ||
            (last && (text[i + 2u] != '=' || text[i + 3u] != '=' ||
                      (b & 0x0Fu) != 0))) {
            return false;
        }
        const unsigned triple = (static_cast<unsigned>(a) << 18) |
                                (static_cast<unsigned>(b) << 12) |
                                (static_cast<unsigned>(c) << 6) |
                                static_cast<unsigned>(d);
        output->push_back(static_cast<std::uint8_t>(triple >> 16));
        if (!last) {
            output->push_back(static_cast<std::uint8_t>(triple >> 8));
            output->push_back(static_cast<std::uint8_t>(triple));
        }
    }
    return output->size() == 64u;
}

Result Rejected(const char* reason)
{
    Result result{};
    result.reason = reason;
    return result;
}

bool IsSofMarker(unsigned char marker)
{
    return (marker >= 0xC0u && marker <= 0xC3u) ||
           (marker >= 0xC5u && marker <= 0xC7u) ||
           (marker >= 0xC9u && marker <= 0xCBu) ||
           (marker >= 0xCDu && marker <= 0xCFu);
}

bool JpegDimensions(const std::string& bytes, int* width, int* height)
{
    if (bytes.size() < 2u || static_cast<unsigned char>(bytes[0]) != 0xFFu ||
        static_cast<unsigned char>(bytes[1]) != 0xD8u) {
        return false;
    }
    std::size_t pos = 2u;
    while (pos < bytes.size()) {
        if (static_cast<unsigned char>(bytes[pos++]) != 0xFFu) {
            return false;
        }
        while (pos < bytes.size() && static_cast<unsigned char>(bytes[pos]) == 0xFFu) {
            ++pos;
        }
        if (pos >= bytes.size()) {
            return false;
        }
        const unsigned char marker = static_cast<unsigned char>(bytes[pos++]);
        if (marker == 0x00u) {
            return false;
        }
        if (marker == 0xDAu || marker == 0xD9u) {
            return false;
        }
        if (marker == 0xD8u || marker == 0x01u ||
            (marker >= 0xD0u && marker <= 0xD7u)) {
            continue;
        }
        if (bytes.size() - pos < 2u) {
            return false;
        }
        const std::size_t segmentLength =
            (static_cast<std::size_t>(static_cast<unsigned char>(bytes[pos])) << 8) |
            static_cast<std::size_t>(static_cast<unsigned char>(bytes[pos + 1u]));
        if (segmentLength < 2u || segmentLength > bytes.size() - pos) {
            return false;
        }
        if (IsSofMarker(marker)) {
            if (segmentLength < 7u || bytes.size() - pos < 7u) {
                return false;
            }
            *height = (static_cast<int>(static_cast<unsigned char>(bytes[pos + 3u])) << 8) |
                      static_cast<int>(static_cast<unsigned char>(bytes[pos + 4u]));
            *width = (static_cast<int>(static_cast<unsigned char>(bytes[pos + 5u])) << 8) |
                     static_cast<int>(static_cast<unsigned char>(bytes[pos + 6u]));
            return *width > 0 && *height > 0;
        }
        pos += segmentLength;
    }
    return false;
}

std::uint32_t Le32(const std::string& bytes, std::size_t pos)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[pos])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[pos + 1u])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[pos + 2u])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[pos + 3u])) << 24);
}

bool FourCc(const std::string& bytes, std::size_t pos, const char* fourcc)
{
    return std::memcmp(bytes.data() + pos, fourcc, 4u) == 0;
}

bool WebpDimensions(const std::string& bytes, int* width, int* height,
                    bool* animated)
{
    if (bytes.size() < 12u || !FourCc(bytes, 0u, "RIFF") ||
        !FourCc(bytes, 8u, "WEBP")) {
        return false;
    }
    const std::uint32_t riffSize = Le32(bytes, 4u);
    (void)riffSize;
    std::size_t pos = 12u;
    bool first = true;
    bool dimensionsFound = false;
    bool animationFound = false;
    while (pos < bytes.size()) {
        if (bytes.size() - pos < 8u) {
            return false;
        }
        const std::uint32_t chunkSize = Le32(bytes, pos + 4u);
        const std::size_t dataStart = pos + 8u;
        if (static_cast<std::uint64_t>(chunkSize) > bytes.size() - dataStart) {
            return false;
        }
        const std::size_t dataEnd = dataStart + static_cast<std::size_t>(chunkSize);
        const bool hasPad = (chunkSize & 1u) != 0u;
        if (FourCc(bytes, pos, "ANIM") || FourCc(bytes, pos, "ANMF")) {
            animationFound = true;
        }
        if (FourCc(bytes, pos, "VP8X")) {
            if (chunkSize >= 1u && (static_cast<unsigned char>(bytes[dataStart]) & 0x02u) != 0u) {
                animationFound = true;
            }
        }
        if (first) {
            if (FourCc(bytes, pos, "VP8 ")) {
                if (chunkSize < 10u || static_cast<unsigned char>(bytes[dataStart + 3u]) != 0x9Du ||
                    static_cast<unsigned char>(bytes[dataStart + 4u]) != 0x01u ||
                    static_cast<unsigned char>(bytes[dataStart + 5u]) != 0x2Au) {
                    return false;
                }
                *width = static_cast<int>(
                    (static_cast<unsigned>(static_cast<unsigned char>(bytes[dataStart + 6u])) |
                     (static_cast<unsigned>(static_cast<unsigned char>(bytes[dataStart + 7u])) << 8)) &
                    0x3FFFu);
                *height = static_cast<int>(
                    (static_cast<unsigned>(static_cast<unsigned char>(bytes[dataStart + 8u])) |
                     (static_cast<unsigned>(static_cast<unsigned char>(bytes[dataStart + 9u])) << 8)) &
                    0x3FFFu);
                dimensionsFound = *width > 0 && *height > 0;
            } else if (FourCc(bytes, pos, "VP8L")) {
                if (chunkSize < 5u || static_cast<unsigned char>(bytes[dataStart]) != 0x2Fu) {
                    return false;
                }
                const std::uint32_t packed = Le32(bytes, dataStart + 1u);
                *width = static_cast<int>((packed & 0x3FFFu) + 1u);
                *height = static_cast<int>(((packed >> 14) & 0x3FFFu) + 1u);
                dimensionsFound = true;
            } else if (FourCc(bytes, pos, "VP8X")) {
                if (chunkSize < 10u) {
                    return false;
                }
                const std::uint32_t widthMinusOne =
                    static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[dataStart + 4u])) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[dataStart + 5u])) << 8) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[dataStart + 6u])) << 16);
                const std::uint32_t heightMinusOne =
                    static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[dataStart + 7u])) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[dataStart + 8u])) << 8) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[dataStart + 9u])) << 16);
                *width = static_cast<int>(widthMinusOne + 1u);
                *height = static_cast<int>(heightMinusOne + 1u);
                dimensionsFound = true;
            } else {
                return false;
            }
            first = false;
        }
        // A final odd-sized chunk may omit its pad byte; any other trailing
        // bytes remain for the next iteration and therefore fail dimensions.
        pos = dataEnd;
        if (hasPad && pos < bytes.size()) {
            ++pos;
        }
    }
    *animated = animationFound;
    return dimensionsFound;
}

} // namespace

bool VerifyP256(const std::uint8_t (&publicKey65)[65], const std::string& message,
               const std::uint8_t sig64[64])
{
    try {
        return VerifyRaw(publicKey65, message, sig64);
    } catch (...) {
        return false;
    }
}

std::string KeyIdOf(const std::uint8_t (&publicKey65)[65])
{
    try {
        std::uint8_t digest[32]{};
        if (!Sha256(publicKey65, 65u, digest)) {
            return std::string();
        }
        return Hex(digest, 8u);
    } catch (...) {
        return std::string();
    }
}

const char* CheckPhoto(const std::string& bytes, bool jpg,
                       const std::string& sha256Hex, int width, int height)
{
    try {
        if (bytes.empty() || bytes.size() > kMaxPhotoBytes) {
            return "photo-size";
        }
        std::uint8_t digest[32]{};
        if (!Sha256(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), digest) ||
            !IsLowerHex64(sha256Hex) || Hex(digest, 32u) != sha256Hex) {
            return "photo-hash";
        }
        int actualWidth = 0;
        int actualHeight = 0;
        if (jpg) {
            if (!JpegDimensions(bytes, &actualWidth, &actualHeight)) {
                return "photo-dims";
            }
        } else {
            bool animated = false;
            if (!WebpDimensions(bytes, &actualWidth, &actualHeight, &animated)) {
                return "photo-dims";
            }
            if (actualWidth != width || actualHeight != height) {
                return "photo-dims";
            }
            if (animated) {
                return "photo-anim";
            }
            return nullptr;
        }
        return actualWidth != width || actualHeight != height ? "photo-dims" : nullptr;
    } catch (...) {
        return "photo-dims";
    }
}

Result Evaluate(const std::string& signedBytes, const Context& context)
{
    Result result{};
    result.action = Action::Reject;
    try {
        if (signedBytes.size() > kMaxSignedBytes) {
            return Rejected("size");
        }
        const std::size_t lineFeed = signedBytes.find('\n');
        if (lineFeed == std::string::npos || lineFeed > 100u) {
            return Rejected("sigline");
        }
        std::vector<std::uint8_t> signature;
        if (!Base64Line(signedBytes.substr(0, lineFeed), &signature) ||
            signature.size() != 64u) {
            return Rejected("sigline");
        }
        const std::string jsonBytes = signedBytes.substr(lineFeed + 1u);
        const std::string signedMessage = std::string(kSigPrefix) + jsonBytes;
        if (context.publicKey65 == nullptr ||
            !VerifyRaw(*context.publicKey65, signedMessage, signature.data())) {
            return Rejected("sig");
        }
        if (!DecodeUtf8(jsonBytes, nullptr, nullptr)) {
            return Rejected("utf8");
        }
        json::Value root;
        int observedDepth = 0;
        const char* parseReason = json::Parse(jsonBytes, 32, &root, &observedDepth);
        if (parseReason != nullptr) {
            return Rejected(parseReason);
        }
        if (root.type != json::Type::Object) {
            return Rejected("json");
        }
        if (observedDepth > 3) {
            return Rejected("depth");
        }

        const json::Value* schema = Field(root, "schema");
        std::uint64_t schemaValue = 0;
        if (!ParseUnsignedInteger(schema, &schemaValue) || schemaValue != 1u) {
            return Rejected("schema");
        }
        const json::Value* keyId = Field(root, "keyId");
        if (keyId == nullptr || keyId->type != json::Type::String ||
            keyId->text != KeyIdOf(*context.publicKey65)) {
            return Rejected("keyid");
        }

        const json::Value* revision = Field(root, "revision");
        const json::Value* issued = Field(root, "issued");
        const json::Value* expires = Field(root, "expires");
        const json::Value* enabled = Field(root, "enabled");
        std::uint64_t revisionValue = 0;
        if (!ParseUnsignedInteger(revision, &revisionValue) || revisionValue < 1u ||
            revisionValue > static_cast<std::uint64_t>(kMaxRevision) ||
            issued == nullptr || issued->type != json::Type::String ||
            expires == nullptr || expires->type != json::Type::String ||
            enabled == nullptr || enabled->type != json::Type::Bool ||
            !ParseTimestamp(issued->text, &result.banner.issuedMs) ||
            !ParseTimestamp(expires->text, &result.banner.expiresMs)) {
            return Rejected("field");
        }
        result.banner.revision = static_cast<std::int64_t>(revisionValue);
        result.banner.enabled = enabled->b;

        if (result.banner.enabled) {
            const json::Value* headline = Field(root, "headline");
            const json::Value* wordmark = Field(root, "wordmark");
            const json::Value* tagline = Field(root, "tagline");
            const json::Value* effects = Field(root, "effects");
            const json::Value* link = Field(root, "link");
            const json::Value* qr = Field(root, "qr");
            const json::Value* photo = Field(root, "photo");
            const json::Value* labels = Field(root, "labels");
            const json::Value* headlineEm = Field(root, "headlineEm");
            const json::Value* badge = Field(root, "badge");
            if (headline == nullptr || headline->type != json::Type::String ||
                wordmark == nullptr || wordmark->type != json::Type::String ||
                tagline == nullptr || tagline->type != json::Type::Array ||
                tagline->items.size() > 2u || effects == nullptr ||
                effects->type != json::Type::Object || link == nullptr ||
                link->type != json::Type::String || qr == nullptr ||
                qr->type != json::Type::Object || photo == nullptr ||
                photo->type != json::Type::Object || labels == nullptr ||
                labels->type != json::Type::Object ||
                (headlineEm != nullptr && headlineEm->type != json::Type::String) ||
                (badge != nullptr && badge->type != json::Type::String)) {
                return Rejected("field");
            }
            for (const auto& part : tagline->items) {
                if (part.type != json::Type::String) {
                    return Rejected("field");
                }
            }
            const json::Value* viewfinder = Field(*effects, "viewfinder");
            const json::Value* steam = Field(*effects, "steam");
            if (viewfinder == nullptr || viewfinder->type != json::Type::Bool ||
                steam == nullptr || steam->type != json::Type::Bool) {
                return Rejected("field");
            }
            const json::Value* qrText = Field(*qr, "text");
            const json::Value* qrSize = Field(*qr, "size");
            const json::Value* qrRows = Field(*qr, "rows");
            if (qrText == nullptr || qrText->type != json::Type::String ||
                qrSize == nullptr || qrSize->type != json::Type::Number ||
                !IsIntegerToken(qrSize->text) ||
                qrRows == nullptr || qrRows->type != json::Type::Array) {
                return Rejected("field");
            }
            for (const auto& row : qrRows->items) {
                if (row.type != json::Type::String) {
                    return Rejected("field");
                }
            }
            const json::Value* webp = Field(*photo, "webp");
            const json::Value* jpg = Field(*photo, "jpg");
            const json::Value* hashes = Field(*photo, "sha256");
            const json::Value* photoWidth = Field(*photo, "w");
            const json::Value* photoHeight = Field(*photo, "h");
            const json::Value* focus = Field(*photo, "focus");
            const json::Value* textBox = Field(*photo, "textBox");
            const json::Value* steamAt = Field(*photo, "steamAt");
            if (webp == nullptr || webp->type != json::Type::String ||
                jpg == nullptr || jpg->type != json::Type::String ||
                hashes == nullptr || hashes->type != json::Type::Object ||
                photoWidth == nullptr || photoHeight == nullptr ||
                focus == nullptr || textBox == nullptr || steamAt == nullptr) {
                return Rejected("field");
            }
            const json::Value* hashWebp = Field(*hashes, "webp");
            const json::Value* hashJpg = Field(*hashes, "jpg");
            std::int64_t parsedWidth = 0;
            std::int64_t parsedHeight = 0;
            if (hashWebp == nullptr || hashWebp->type != json::Type::String ||
                hashJpg == nullptr || hashJpg->type != json::Type::String ||
                !IsLowerHex64(hashWebp->text) || !IsLowerHex64(hashJpg->text) ||
                !ParseInt64(photoWidth, &parsedWidth) || parsedWidth < 1 || parsedWidth > 1600 ||
                !ParseInt64(photoHeight, &parsedHeight) || parsedHeight < 1 || parsedHeight > 1000 ||
                !ParseArrayNumbers(focus, 2u, result.banner.focus) ||
                !ParseArrayNumbers(textBox, 4u, result.banner.textBox) ||
                !ParseArrayNumbers(steamAt, 2u, result.banner.steamAt) ||
                result.banner.textBox[0] + result.banner.textBox[2] > 1.0 ||
                result.banner.textBox[1] + result.banner.textBox[3] > 1.0 ||
                !PhotoName(webp->text, ".webp") || !PhotoName(jpg->text, ".jpg")) {
                return Rejected("field");
            }
            const json::Value* labelLink = Field(*labels, "link");
            const json::Value* labelQr = Field(*labels, "qr");
            if (labelLink == nullptr || labelLink->type != json::Type::String ||
                labelQr == nullptr || labelQr->type != json::Type::String) {
                return Rejected("field");
            }
            result.banner.headline = std::wstring();
            result.banner.wordmark = std::wstring();
            result.banner.labelLink = std::wstring();
            result.banner.labelQr = std::wstring();
            if (!CheckText(headline, 1u, 48u, &result.banner.headline) ||
                !CheckText(wordmark, 1u, 12u, &result.banner.wordmark) ||
                !CheckText(labelLink, 1u, 80u, &result.banner.labelLink) ||
                !CheckText(labelQr, 1u, 80u, &result.banner.labelQr)) {
                return Rejected("text");
            }
            for (const auto& part : tagline->items) {
                std::wstring partWide;
                if (!CheckText(&part, 1u, 18u, &partWide)) {
                    return Rejected("text");
                }
                result.banner.tagline.push_back(std::move(partWide));
            }
            if (headlineEm != nullptr) {
                result.banner.hasHeadlineEm = true;
                if (!CheckText(headlineEm, 1u, 48u, &result.banner.headlineEm) ||
                    result.banner.headline.find(result.banner.headlineEm) == std::wstring::npos) {
                    return Rejected("text");
                }
            }
            if (badge != nullptr) {
                result.banner.hasBadge = true;
                if (!CheckText(badge, 1u, 16u, &result.banner.badge)) {
                    return Rejected("text");
                }
            }
            result.banner.link = link->text;
            result.banner.qrText = qrText->text;
            if (!Url(result.banner.link) || !Url(result.banner.qrText)) {
                return Rejected("url");
            }
            result.banner.qrRows.clear();
            if (qrSize->text != "37" || qrRows->items.size() != 37u) {
                return Rejected("qr");
            }
            for (const auto& row : qrRows->items) {
                if (row.text.size() != 37u) {
                    return Rejected("qr");
                }
                for (const char ch : row.text) {
                    if (ch != '0' && ch != '1') {
                        return Rejected("qr");
                    }
                }
                result.banner.qrRows.push_back(row.text);
            }
            result.banner.viewfinder = viewfinder->b;
            result.banner.steam = steam->b;
            result.banner.photoWebp = webp->text;
            result.banner.photoJpg = jpg->text;
            result.banner.sha256Webp = hashWebp->text;
            result.banner.sha256Jpg = hashJpg->text;
            result.banner.w = static_cast<int>(parsedWidth);
            result.banner.h = static_cast<int>(parsedHeight);
        }

        if (AfterPlusDay(result.banner.issuedMs, context.nowMs, kDayMs) ||
            context.nowMs >= result.banner.expiresMs ||
            result.banner.expiresMs - result.banner.issuedMs > 400ll * kDayMs ||
            result.banner.expiresMs <= result.banner.issuedMs) {
            return Rejected("time");
        }
        if (context.hwm < 0 || context.hwm > kMaxRevision ||
            result.banner.revision < context.hwm) {
            return Rejected("revision");
        }
        if (context.floor < 0 || context.floor > kMaxRevision ||
            result.banner.revision < context.floor) {
            return Rejected("floor");
        }
        if (result.banner.revision == context.hwm && context.storedSigned != nullptr &&
            signedBytes != *context.storedSigned) {
            return Rejected("equal");
        }

        std::uint8_t digest[32]{};
        if (!Sha256(reinterpret_cast<const std::uint8_t*>(signedBytes.data()),
                    signedBytes.size(), digest)) {
            return Rejected("sig");
        }
        result.sha256 = Hex(digest, 32u);
        result.action = result.banner.enabled ? Action::Show : Action::KillSwitch;
        result.reason = nullptr;
        return result;
    } catch (...) {
        return Rejected("json");
    }
}

} // namespace ob
