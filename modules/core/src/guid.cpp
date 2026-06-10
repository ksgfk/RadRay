#include <radray/guid.h>

#include <stdexcept>

#if defined(_WIN32) || defined(_WIN64)
#include <radray/platform/win32_headers.h>
#include <objbase.h>
#elif defined(__linux__)
#include <cerrno>
#include <sys/types.h>
#include <sys/random.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <cstdlib>
#else
#include <random>
#endif

#include <fmt/format.h>

#include <radray/hash.h>

namespace radray {
namespace {

char NormalizeFormat(char format) noexcept {
    if (format >= 'a' && format <= 'z') {
        return static_cast<char>(format - ('a' - 'A'));
    }
    return format;
}

bool IsAsciiSpace(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && IsAsciiSpace(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && IsAsciiSpace(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

int HexValue(char ch) noexcept {
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

bool TryParseByte(std::string_view value, size_t offset, uint8_t& byte) noexcept {
    const int high = HexValue(value[offset]);
    const int low = HexValue(value[offset + 1]);
    if (high < 0 || low < 0) {
        return false;
    }

    byte = static_cast<uint8_t>((high << 4) | low);
    return true;
}

bool TryParseN(std::string_view value, Guid& result) noexcept {
    Guid::ByteArray bytes{};
    for (size_t index = 0; index < Guid::Size; ++index) {
        if (!TryParseByte(value, index * 2, bytes[index])) {
            return false;
        }
    }

    result = Guid{bytes};
    return true;
}

bool TryParseD(std::string_view value, Guid& result) noexcept {
    if (value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-') {
        return false;
    }

    Guid::ByteArray bytes{};
    size_t inputOffset = 0;
    for (size_t index = 0; index < Guid::Size; ++index) {
        if (inputOffset == 8 || inputOffset == 13 || inputOffset == 18 || inputOffset == 23) {
            ++inputOffset;
        }
        if (!TryParseByte(value, inputOffset, bytes[index])) {
            return false;
        }
        inputOffset += 2;
    }

    result = Guid{bytes};
    return true;
}

void AppendHexByte(string& output, uint8_t byte) {
    const char hex[] = "0123456789abcdef";
    output.push_back(hex[(byte >> 4) & 0x0f]);
    output.push_back(hex[byte & 0x0f]);
}

void AppendN(string& output, const Guid::ByteArray& bytes) {
    for (uint8_t byte : bytes) {
        AppendHexByte(output, byte);
    }
}

void AppendD(string& output, const Guid::ByteArray& bytes) {
    for (size_t index = 0; index < Guid::Size; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output.push_back('-');
        }
        AppendHexByte(output, bytes[index]);
    }
}

#if !defined(_WIN32) && !defined(_WIN64)
void FillRandomBytes(Guid::ByteArray& bytes) {
#if defined(__linux__)
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(fmt::format("getrandom failed with errno {}", errno));
        }
        offset += static_cast<size_t>(count);
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    ::arc4random_buf(bytes.data(), bytes.size());
#else
    std::random_device randomDevice;
    for (uint8_t& byte : bytes) {
        byte = static_cast<uint8_t>(randomDevice());
    }
#endif
}
#endif

}  // namespace

Guid Guid::NewGuid() {
#if defined(_WIN32) || defined(_WIN64)
    Guid result;
    do {
        ::GUID value{};
        const HRESULT hr = ::CoCreateGuid(&value);
        if (FAILED(hr)) {
            throw std::runtime_error(fmt::format("CoCreateGuid failed with HRESULT 0x{:08x}", static_cast<uint32_t>(hr)));
        }

        result = Guid{
            value.Data1,
            value.Data2,
            value.Data3,
            value.Data4[0],
            value.Data4[1],
            value.Data4[2],
            value.Data4[3],
            value.Data4[4],
            value.Data4[5],
            value.Data4[6],
            value.Data4[7],
        };
    } while (result.IsEmpty());
    return result;
#else
    Guid result;
    do {
        auto bytes = result.Bytes();
        FillRandomBytes(bytes);
        bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
        bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
        result = Guid{bytes};
    } while (result.IsEmpty());
    return result;
#endif
}

Guid Guid::Parse(std::string_view value) {
    Guid result;
    if (!TryParse(value, result)) {
        throw std::invalid_argument("Invalid GUID string.");
    }
    return result;
}

bool Guid::TryParse(std::string_view value, Guid& result) noexcept {
    value = Trim(value);

    if (value.size() == 32) {
        return TryParseN(value, result);
    }
    if (value.size() == 36) {
        return TryParseD(value, result);
    }
    if (value.size() == 38) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '{' && last == '}') || (first == '(' && last == ')')) {
            return TryParseD(value.substr(1, 36), result);
        }
    }

    return false;
}

string Guid::ToString() const {
    return ToString('D');
}

string Guid::ToString(char format) const {
    format = NormalizeFormat(format);

    switch (format) {
    case 'N': {
        string output;
        output.reserve(32);
        AppendN(output, _bytes);
        return output;
    }
    case 'D': {
        string output;
        output.reserve(36);
        AppendD(output, _bytes);
        return output;
    }
    case 'B': {
        string output;
        output.reserve(38);
        output.push_back('{');
        AppendD(output, _bytes);
        output.push_back('}');
        return output;
    }
    case 'P': {
        string output;
        output.reserve(38);
        output.push_back('(');
        AppendD(output, _bytes);
        output.push_back(')');
        return output;
    }
    default:
        throw std::invalid_argument("Invalid GUID format. Expected N, D, B, or P.");
    }
}

string format_as(const Guid& value) {
    return value.ToString();
}

}  // namespace radray

std::size_t std::hash<radray::Guid>::operator()(const radray::Guid& value) const noexcept {
    const auto& bytes = value.Bytes();
    return radray::HashData(bytes.data(), bytes.size());
}
