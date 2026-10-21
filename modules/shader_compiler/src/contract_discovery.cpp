#include <radray/shader_compiler/contract_discovery.h>

#include <algorithm>
#include <cstring>
#include <tuple>
#include <string_view>

namespace radray::shader_compiler {
namespace {

using std::string_view;

using shader::EntryPoint;
using shader::KeywordGroup;
using shader::ShaderKind;
using shader::ShaderStage;

void HashByte(uint64_t& first, uint64_t& second, uint8_t value) noexcept {
    first ^= value;
    first *= 1099511628211ull;
    second ^= static_cast<uint64_t>(value) + 0x9e3779b97f4a7c15ull;
    second *= 14029467366897019727ull;
}

shader::ContractHash MakeContractHash(const shader::ShaderContract& contract) {
    uint64_t first = 1469598103934665603ull;
    uint64_t second = 1099511628211ull;
    auto addText = [&](string_view value) {
        for (const char character : value) {
            HashByte(first, second, static_cast<uint8_t>(character));
        }
        HashByte(first, second, 0xff);
    };
    addText(contract.Kind == ShaderKind::Graphics ? "graphics" : "compute");
    for (const KeywordGroup& group : contract.KeywordGroups) {
        addText(group.Name);
        for (const string& value : group.Values) {
            addText(value);
        }
    }
    for (const EntryPoint& entry : contract.EntryPoints) {
        addText(entry.Name);
        HashByte(first, second, static_cast<uint8_t>(entry.Stage));
    }
    shader::ContractHash hash{};
    for (uint32_t index = 0; index < 8; ++index) {
        hash.Bytes[index] = static_cast<uint8_t>(first >> (index * 8));
        hash.Bytes[index + 8] = static_cast<uint8_t>(second >> (index * 8));
    }
    return hash;
}

}  // namespace

namespace {

class ContractWireReader {
public:
    explicit ContractWireReader(std::span<const byte> bytes) noexcept : _bytes(bytes) {}

    bool ReadU8(uint8_t& value) noexcept {
        if (_offset >= _bytes.size()) {
            return false;
        }
        value = static_cast<uint8_t>(_bytes[_offset++]);
        return true;
    }

    bool ReadU16(uint16_t& value) noexcept {
        uint8_t first = 0;
        uint8_t second = 0;
        if (!ReadU8(first) || !ReadU8(second)) {
            return false;
        }
        value = static_cast<uint16_t>(first | (static_cast<uint16_t>(second) << 8));
        return true;
    }

    bool ReadU32(uint32_t& value) noexcept {
        uint8_t bytes[4]{};
        for (uint8_t& byteValue : bytes) {
            if (!ReadU8(byteValue)) {
                return false;
            }
        }
        value = static_cast<uint32_t>(bytes[0]) |
                (static_cast<uint32_t>(bytes[1]) << 8) |
                (static_cast<uint32_t>(bytes[2]) << 16) |
                (static_cast<uint32_t>(bytes[3]) << 24);
        return true;
    }

    bool ReadString(string& value) {
        uint32_t size = 0;
        if (!ReadU32(size) || size > _bytes.size() - _offset) {
            return false;
        }
        value.assign(
            reinterpret_cast<const char*>(_bytes.data() + _offset),
            static_cast<size_t>(size));
        _offset += size;
        return true;
    }

    size_t Remaining() const noexcept { return _bytes.size() - _offset; }

    bool ReadHash(shader::Hash128& value) noexcept {
        if (_bytes.size() - _offset < value.Bytes.size()) {
            return false;
        }
        std::memcpy(value.Bytes.data(), _bytes.data() + _offset, value.Bytes.size());
        _offset += value.Bytes.size();
        return true;
    }

private:
    std::span<const byte> _bytes;
    size_t _offset{0};
};

void AddWireDiagnostic(DiscoveryResult& result, string_view message) {
    result.Status = shader::CompileStatus::InvalidRequest;
    result.Diagnostics.push_back({2200, string{message}});
}

bool IsCanonicalContract(const shader::ShaderContract& contract) noexcept {
    for (size_t index = 1; index < contract.KeywordGroups.size(); ++index) {
        if (contract.KeywordGroups[index - 1].Name >= contract.KeywordGroups[index].Name) {
            return false;
        }
    }
    for (const shader::KeywordGroup& group : contract.KeywordGroups) {
        if (group.Name.empty() || group.Values.empty()) {
            return false;
        }
        for (size_t index = 1; index < group.Values.size(); ++index) {
            if (group.Values[index - 1] >= group.Values[index]) {
                return false;
            }
        }
    }
    for (size_t index = 1; index < contract.EntryPoints.size(); ++index) {
        const auto& previous = contract.EntryPoints[index - 1];
        const auto& current = contract.EntryPoints[index];
        if (std::tie(previous.Stage, previous.Name) >= std::tie(current.Stage, current.Name)) {
            return false;
        }
    }
    uint32_t vertexCount = 0;
    uint32_t pixelCount = 0;
    uint32_t computeCount = 0;
    for (const shader::EntryPoint& entry : contract.EntryPoints) {
        if (entry.Name.empty()) {
            return false;
        }
        switch (entry.Stage) {
            case shader::ShaderStage::Vertex:
                ++vertexCount;
                break;
            case shader::ShaderStage::Pixel:
                ++pixelCount;
                break;
            case shader::ShaderStage::Compute:
                ++computeCount;
                break;
            default:
                return false;
        }
    }
    return (contract.Kind == shader::ShaderKind::Compute && computeCount == 1 &&
            vertexCount == 0 && pixelCount == 0) ||
           (contract.Kind == shader::ShaderKind::Graphics && vertexCount == 1 &&
            pixelCount <= 1 && computeCount == 0);
}

}  // namespace

DiscoveryResult DecodeWireShaderContract(std::span<const byte> blob) {
    DiscoveryResult result;
    ContractWireReader reader{blob};
    uint32_t magic = 0;
    uint16_t schema = 0;
    uint8_t kind = 0;
    uint8_t reserved = 0;
    uint16_t reserved2 = 0;
    uint32_t groupCount = 0;
    if (!reader.ReadU32(magic) || !reader.ReadU16(schema) || !reader.ReadU8(kind) ||
        !reader.ReadU8(reserved) || !reader.ReadU16(reserved2) || !reader.ReadU32(groupCount) ||
        magic != shader::kShaderContractWireMagic ||
        schema != shader::kShaderContractWireSchemaVersion || reserved != 0 || reserved2 != 0 ||
        groupCount > 1024) {
        AddWireDiagnostic(result, "contract blob header is invalid");
        return result;
    }
    if (kind > static_cast<uint8_t>(shader::ShaderKind::Compute)) {
        AddWireDiagnostic(result, "contract blob kind is invalid");
        return result;
    }
    result.Contract.Kind = static_cast<shader::ShaderKind>(kind);
    for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
        shader::KeywordGroup group;
        uint32_t valueCount = 0;
        if (!reader.ReadString(group.Name) || !reader.ReadU32(valueCount) || valueCount == 0 ||
            valueCount > 1024) {
            AddWireDiagnostic(result, "contract blob keyword group is invalid");
            return result;
        }
        group.Values.reserve(valueCount);
        for (uint32_t valueIndex = 0; valueIndex < valueCount; ++valueIndex) {
            string value;
            if (!reader.ReadString(value) || value.empty()) {
                AddWireDiagnostic(result, "contract blob keyword value is invalid");
                return result;
            }
            group.Values.push_back(std::move(value));
        }
        result.Contract.KeywordGroups.push_back(std::move(group));
    }

    uint32_t entryCount = 0;
    if (!reader.ReadU32(entryCount) || entryCount == 0 || entryCount > 16) {
        AddWireDiagnostic(result, "contract blob entry list is invalid");
        return result;
    }
    result.Contract.EntryPoints.reserve(entryCount);
    for (uint32_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
        shader::EntryPoint entry;
        uint8_t stage = 0;
        uint8_t entryReserved = 0;
        uint16_t entryReserved2 = 0;
        if (!reader.ReadString(entry.Name) || !reader.ReadU8(stage) || !reader.ReadU8(entryReserved) ||
            !reader.ReadU16(entryReserved2) || entryReserved != 0 || entryReserved2 != 0 ||
            stage > static_cast<uint8_t>(shader::ShaderStage::Compute)) {
            AddWireDiagnostic(result, "contract blob entry is invalid");
            return result;
        }
        entry.Stage = static_cast<shader::ShaderStage>(stage);
        result.Contract.EntryPoints.push_back(std::move(entry));
    }

    shader::ContractHash encodedHash{};
    if (!reader.ReadHash(encodedHash) || reader.Remaining() != 0 || !IsCanonicalContract(result.Contract) ||
        MakeContractHash(result.Contract) != encodedHash) {
        AddWireDiagnostic(result, "contract blob canonical hash or topology is invalid");
        return result;
    }
    result.Contract.Hash = encodedHash;
    result.Status = shader::CompileStatus::Success;
    return result;
}

}  // namespace radray::shader_compiler
