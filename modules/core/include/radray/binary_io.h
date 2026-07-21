#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <radray/types.h>

namespace radray {

// Fixed-width scalar values are encoded in little-endian byte order.
class BinaryWriter {
public:
    BinaryWriter() = default;
    explicit BinaryWriter(size_t reservedSize);

    void ReserveAdditional(size_t size);

    void U8(uint8_t value);
    void U32(uint32_t value);
    void U64(uint64_t value);
    void Size32(size_t value);
    void I32(int32_t value);
    void Float(float value);
    void Bool(bool value);
    void Bytes(std::span<const byte> value);
    void SizedBytes(std::span<const byte> value);
    void String(std::string_view value);

    size_t GetSize() const noexcept { return _data.size(); }
    bool IsEmpty() const noexcept { return _data.empty(); }
    std::span<const byte> GetData() const noexcept { return _data; }
    vector<byte> TakeData() && noexcept;

private:
    vector<byte> _data;
};

// Failed reads leave the cursor and destination value unchanged.
class BinaryReader {
public:
    explicit BinaryReader(std::span<const byte> data) noexcept;

    bool U8(uint8_t& value) noexcept;
    bool U32(uint32_t& value) noexcept;
    bool U64(uint64_t& value) noexcept;
    bool I32(int32_t& value) noexcept;
    bool Float(float& value) noexcept;
    bool Bool(bool& value) noexcept;
    bool Bytes(size_t size, std::span<const byte>& value) noexcept;
    bool SizedBytes(std::span<const byte>& value) noexcept;
    bool String(std::string_view& value) noexcept;

    size_t Remaining() const noexcept { return _data.size() - _offset; }
    bool AtEnd() const noexcept { return _offset == _data.size(); }

private:
    std::span<const byte> _data;
    size_t _offset{0};
};

}  // namespace radray
