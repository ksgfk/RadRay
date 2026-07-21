#include <radray/binary_io.h>

#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace radray {

BinaryWriter::BinaryWriter(size_t reservedSize) {
    _data.reserve(reservedSize);
}

void BinaryWriter::ReserveAdditional(size_t size) {
    if (size > _data.max_size() - _data.size()) {
        throw std::length_error{"binary buffer is too large"};
    }
    _data.reserve(_data.size() + size);
}

void BinaryWriter::U8(uint8_t value) {
    _data.emplace_back(static_cast<byte>(value));
}

void BinaryWriter::U32(uint32_t value) {
    const std::array<byte, 4> bytes{
        static_cast<byte>(value & 0xffu),
        static_cast<byte>((value >> 8) & 0xffu),
        static_cast<byte>((value >> 16) & 0xffu),
        static_cast<byte>((value >> 24) & 0xffu),
    };
    Bytes(bytes);
}

void BinaryWriter::U64(uint64_t value) {
    const std::array<byte, 8> bytes{
        static_cast<byte>(value & 0xffu),
        static_cast<byte>((value >> 8) & 0xffu),
        static_cast<byte>((value >> 16) & 0xffu),
        static_cast<byte>((value >> 24) & 0xffu),
        static_cast<byte>((value >> 32) & 0xffu),
        static_cast<byte>((value >> 40) & 0xffu),
        static_cast<byte>((value >> 48) & 0xffu),
        static_cast<byte>((value >> 56) & 0xffu),
    };
    Bytes(bytes);
}

void BinaryWriter::Size32(size_t value) {
    if (value > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error{"binary size does not fit in uint32_t"};
    }
    U32(static_cast<uint32_t>(value));
}

void BinaryWriter::I32(int32_t value) {
    U32(std::bit_cast<uint32_t>(value));
}

void BinaryWriter::Float(float value) {
    U32(std::bit_cast<uint32_t>(value));
}

void BinaryWriter::Bool(bool value) {
    U8(value ? 1 : 0);
}

void BinaryWriter::Bytes(std::span<const byte> value) {
    _data.insert(_data.end(), value.begin(), value.end());
}

void BinaryWriter::SizedBytes(std::span<const byte> value) {
    static_assert(sizeof(size_t) <= sizeof(uint64_t));
    U64(static_cast<uint64_t>(value.size()));
    Bytes(value);
}

void BinaryWriter::String(std::string_view value) {
    Size32(value.size());
    Bytes(std::as_bytes(std::span{value.data(), value.size()}));
}

vector<byte> BinaryWriter::TakeData() && noexcept {
    return std::move(_data);
}

BinaryReader::BinaryReader(std::span<const byte> data) noexcept
    : _data(data) {}

bool BinaryReader::U8(uint8_t& value) noexcept {
    if (Remaining() < sizeof(uint8_t)) return false;
    value = std::to_integer<uint8_t>(_data[_offset]);
    ++_offset;
    return true;
}

bool BinaryReader::U32(uint32_t& value) noexcept {
    if (Remaining() < sizeof(uint32_t)) return false;
    uint32_t result = 0;
    for (uint32_t i = 0; i < sizeof(uint32_t); ++i) {
        result |= static_cast<uint32_t>(std::to_integer<uint8_t>(_data[_offset + i])) << (i * 8);
    }
    _offset += sizeof(uint32_t);
    value = result;
    return true;
}

bool BinaryReader::U64(uint64_t& value) noexcept {
    if (Remaining() < sizeof(uint64_t)) return false;
    uint64_t result = 0;
    for (uint32_t i = 0; i < sizeof(uint64_t); ++i) {
        result |= static_cast<uint64_t>(std::to_integer<uint8_t>(_data[_offset + i])) << (i * 8);
    }
    _offset += sizeof(uint64_t);
    value = result;
    return true;
}

bool BinaryReader::I32(int32_t& value) noexcept {
    uint32_t bits = 0;
    if (!U32(bits)) return false;
    value = std::bit_cast<int32_t>(bits);
    return true;
}

bool BinaryReader::Float(float& value) noexcept {
    uint32_t bits = 0;
    if (!U32(bits)) return false;
    value = std::bit_cast<float>(bits);
    return true;
}

bool BinaryReader::Bool(bool& value) noexcept {
    if (Remaining() < sizeof(uint8_t)) return false;
    const uint8_t raw = std::to_integer<uint8_t>(_data[_offset]);
    if (raw > 1) return false;
    ++_offset;
    value = raw != 0;
    return true;
}

bool BinaryReader::Bytes(size_t size, std::span<const byte>& value) noexcept {
    if (size > Remaining()) return false;
    value = _data.subspan(_offset, size);
    _offset += size;
    return true;
}

bool BinaryReader::SizedBytes(std::span<const byte>& value) noexcept {
    BinaryReader cursor = *this;
    uint64_t size = 0;
    if (!cursor.U64(size) || size > cursor.Remaining()) return false;
    std::span<const byte> bytes;
    if (!cursor.Bytes(static_cast<size_t>(size), bytes)) return false;
    value = bytes;
    *this = cursor;
    return true;
}

bool BinaryReader::String(std::string_view& value) noexcept {
    BinaryReader cursor = *this;
    uint32_t size = 0;
    if (!cursor.U32(size) || size > cursor.Remaining()) return false;
    std::span<const byte> bytes;
    if (!cursor.Bytes(size, bytes)) return false;
    value = bytes.empty()
                ? std::string_view{}
                : std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    *this = cursor;
    return true;
}

}  // namespace radray
