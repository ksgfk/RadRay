#include <radray/shader/shader_binary.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>

#include <xxhash.h>

#include <radray/file.h>
#include <radray/logger.h>
#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#endif

namespace radray::shader {
namespace {

constexpr std::array<uint8_t, 8> kMagic{'R', 'D', 'R', 'S', 'H', 'D', 'R', 0};
constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kMaxPassCount = 1u << 16;
constexpr uint32_t kMaxRecordCount = 1u << 20;
constexpr uint32_t kMaxVectorCount = 1u << 20;
constexpr uint32_t kMaxStringSize = 64u << 20;
constexpr uint64_t kMaxBlobSize = 1ull << 30;
constexpr uint64_t kMaxFileSize = 1ull << 30;

class Writer {
public:
    void U8(uint8_t value) { Data.emplace_back(static_cast<byte>(value)); }
    void U32(uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i) U8(static_cast<uint8_t>((value >> (i * 8)) & 0xffu));
    }
    void U64(uint64_t value) {
        for (uint32_t i = 0; i < 8; ++i) U8(static_cast<uint8_t>((value >> (i * 8)) & 0xffu));
    }
    void I32(int32_t value) { U32(std::bit_cast<uint32_t>(value)); }
    void Float(float value) { U32(std::bit_cast<uint32_t>(value)); }
    void Bool(bool value) { U8(value ? 1 : 0); }
    void Bytes(std::span<const byte> value) { Data.insert(Data.end(), value.begin(), value.end()); }
    void SizedBytes(std::span<const byte> value) {
        if (value.size() > kMaxBlobSize) throw std::length_error{"shader blob is too large"};
        U64(static_cast<uint64_t>(value.size()));
        Bytes(value);
    }
    void String(std::string_view value) {
        if (value.size() > kMaxStringSize) throw std::length_error{"shader string is too large"};
        U32(static_cast<uint32_t>(value.size()));
        Bytes(std::as_bytes(std::span{value.data(), value.size()}));
    }

    vector<byte> Data;
};

class Reader {
public:
    explicit Reader(std::span<const byte> data) noexcept : _data(data) {}

    bool U8(uint8_t& value) noexcept {
        if (_offset >= _data.size()) return false;
        value = std::to_integer<uint8_t>(_data[_offset++]);
        return true;
    }
    bool U32(uint32_t& value) noexcept {
        value = 0;
        for (uint32_t i = 0; i < 4; ++i) {
            uint8_t part = 0;
            if (!U8(part)) return false;
            value |= static_cast<uint32_t>(part) << (i * 8);
        }
        return true;
    }
    bool U64(uint64_t& value) noexcept {
        value = 0;
        for (uint32_t i = 0; i < 8; ++i) {
            uint8_t part = 0;
            if (!U8(part)) return false;
            value |= static_cast<uint64_t>(part) << (i * 8);
        }
        return true;
    }
    bool I32(int32_t& value) noexcept {
        uint32_t bits = 0;
        if (!U32(bits)) return false;
        value = std::bit_cast<int32_t>(bits);
        return true;
    }
    bool Float(float& value) noexcept {
        uint32_t bits = 0;
        if (!U32(bits)) return false;
        value = std::bit_cast<float>(bits);
        return true;
    }
    bool Bool(bool& value) noexcept {
        uint8_t raw = 0;
        if (!U8(raw) || raw > 1) return false;
        value = raw != 0;
        return true;
    }
    bool Bytes(size_t size, std::span<const byte>& value) noexcept {
        if (size > Remaining()) return false;
        value = _data.subspan(_offset, size);
        _offset += size;
        return true;
    }
    bool SizedBytes(vector<byte>& value) {
        uint64_t size = 0;
        if (!U64(size) || size > kMaxBlobSize || size > Remaining()) return false;
        std::span<const byte> bytes;
        if (!Bytes(static_cast<size_t>(size), bytes)) return false;
        value.assign(bytes.begin(), bytes.end());
        return true;
    }
    bool String(string& value) {
        uint32_t size = 0;
        if (!U32(size) || size > kMaxStringSize || size > Remaining()) return false;
        std::span<const byte> bytes;
        if (!Bytes(size, bytes)) return false;
        value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }
    size_t Remaining() const noexcept { return _data.size() - _offset; }
    bool AtEnd() const noexcept { return _offset == _data.size(); }

private:
    std::span<const byte> _data;
    size_t _offset{0};
};

template <typename Enum>
void WriteEnum(Writer& writer, Enum value) {
    writer.I32(static_cast<int32_t>(value));
}

template <typename Enum>
bool ReadEnum(Reader& reader, Enum& value, int32_t minValue, int32_t maxValue) noexcept {
    int32_t raw = 0;
    if (!reader.I32(raw) || raw < minValue || raw > maxValue) return false;
    value = static_cast<Enum>(raw);
    return true;
}

template <typename T, typename WriteValue>
void WriteVector(Writer& writer, const vector<T>& values, WriteValue&& writeValue) {
    if (values.size() > kMaxVectorCount) throw std::length_error{"shader vector is too large"};
    writer.U32(static_cast<uint32_t>(values.size()));
    for (const T& value : values) writeValue(writer, value);
}

template <typename T, typename ReadValue>
bool ReadVector(Reader& reader, vector<T>& values, ReadValue&& readValue) {
    uint32_t count = 0;
    if (!reader.U32(count) || count > kMaxVectorCount) return false;
    vector<T> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        T value{};
        if (!readValue(reader, value)) return false;
        result.emplace_back(std::move(value));
    }
    values = std::move(result);
    return true;
}

void WriteHash(Writer& writer, ShaderHash value) {
    writer.U64(value.Low);
    writer.U64(value.High);
}

bool ReadHash(Reader& reader, ShaderHash& value) noexcept {
    return reader.U64(value.Low) && reader.U64(value.High);
}

void WriteGuid(Writer& writer, const Guid& guid) {
    writer.Bytes(std::as_bytes(std::span{guid.Bytes()}));
}

bool ReadGuid(Reader& reader, Guid& guid) noexcept {
    std::span<const byte> bytes;
    if (!reader.Bytes(Guid::Size, bytes)) return false;
    Guid::ByteArray raw{};
    std::memcpy(raw.data(), bytes.data(), raw.size());
    guid = Guid{raw};
    return true;
}

void WriteBlendComponent(Writer& writer, const BlendComponent& value) {
    WriteEnum(writer, value.Src);
    WriteEnum(writer, value.Dst);
    WriteEnum(writer, value.Op);
}

bool ReadBlendComponent(Reader& reader, BlendComponent& value) noexcept {
    return ReadEnum(reader, value.Src, 0, static_cast<int32_t>(BlendFactor::OneMinusSrc1Alpha)) &&
           ReadEnum(reader, value.Dst, 0, static_cast<int32_t>(BlendFactor::OneMinusSrc1Alpha)) &&
           ReadEnum(reader, value.Op, 0, static_cast<int32_t>(BlendOperation::Max));
}

void WriteStencilFace(Writer& writer, const StencilFaceState& value) {
    WriteEnum(writer, value.Compare);
    WriteEnum(writer, value.FailOp);
    WriteEnum(writer, value.DepthFailOp);
    WriteEnum(writer, value.PassOp);
}

bool ReadStencilFace(Reader& reader, StencilFaceState& value) noexcept {
    return ReadEnum(reader, value.Compare, 0, static_cast<int32_t>(CompareFunction::Always)) &&
           ReadEnum(reader, value.FailOp, 0, static_cast<int32_t>(StencilOperation::DecrementWrap)) &&
           ReadEnum(reader, value.DepthFailOp, 0, static_cast<int32_t>(StencilOperation::DecrementWrap)) &&
           ReadEnum(reader, value.PassOp, 0, static_cast<int32_t>(StencilOperation::DecrementWrap));
}

void WriteGraphicsPass(Writer& writer, const ShaderGraphicsPassDesc& value) {
    writer.String(value.VertexEntry);
    writer.Bool(value.PixelEntry.has_value());
    if (value.PixelEntry.has_value()) writer.String(*value.PixelEntry);
    WriteVector(writer, value.ColorTargets, [](Writer& target, const ShaderColorTargetDesc& color) {
        target.U32(color.Index);
        target.Bool(color.Blend.has_value());
        if (color.Blend.has_value()) {
            WriteBlendComponent(target, color.Blend->Color);
            WriteBlendComponent(target, color.Blend->Alpha);
        }
        target.U32(color.WriteMask.value());
    });
    WriteEnum(writer, value.Cull);
    writer.Bool(value.Depth.has_value());
    if (value.Depth.has_value()) WriteEnum(writer, *value.Depth);
    writer.Float(value.DepthBiasFactor);
    writer.Float(value.DepthBiasUnits);
    writer.Bool(value.Stencil.has_value());
    if (value.Stencil.has_value()) {
        writer.U32(value.Stencil->Reference);
        WriteStencilFace(writer, value.Stencil->State.Front);
        WriteStencilFace(writer, value.Stencil->State.Back);
        writer.U32(value.Stencil->State.ReadMask);
        writer.U32(value.Stencil->State.WriteMask);
    }
    writer.Bool(value.DepthWrite);
    writer.Bool(value.DepthClip);
    writer.Bool(value.AlphaToMask);
    writer.Bool(value.ConservativeRasterization);
}

bool ReadGraphicsPass(Reader& reader, ShaderGraphicsPassDesc& value) {
    bool hasPixel = false;
    if (!reader.String(value.VertexEntry) || !reader.Bool(hasPixel)) return false;
    if (hasPixel) {
        string entry;
        if (!reader.String(entry)) return false;
        value.PixelEntry = std::move(entry);
    }
    if (!ReadVector(reader, value.ColorTargets, [](Reader& source, ShaderColorTargetDesc& color) {
            bool hasBlend = false;
            uint32_t writeMask = 0;
            if (!source.U32(color.Index) || !source.Bool(hasBlend)) return false;
            if (hasBlend) {
                BlendState blend;
                if (!ReadBlendComponent(source, blend.Color) || !ReadBlendComponent(source, blend.Alpha)) return false;
                color.Blend = blend;
            }
            if (!source.U32(writeMask) || (writeMask & ~static_cast<uint32_t>(ColorWrite::All)) != 0) return false;
            color.WriteMask = ColorWrites{writeMask};
            return true;
        }) ||
        !ReadEnum(reader, value.Cull, 0, static_cast<int32_t>(CullMode::None))) {
        return false;
    }
    bool hasDepth = false;
    if (!reader.Bool(hasDepth)) return false;
    if (hasDepth) {
        CompareFunction depth{};
        if (!ReadEnum(reader, depth, 0, static_cast<int32_t>(CompareFunction::Always))) return false;
        value.Depth = depth;
    } else {
        value.Depth.reset();
    }
    bool hasStencil = false;
    if (!reader.Float(value.DepthBiasFactor) || !reader.Float(value.DepthBiasUnits) || !reader.Bool(hasStencil)) return false;
    if (hasStencil) {
        ShaderStencilTestDesc stencil;
        if (!reader.U32(stencil.Reference) || !ReadStencilFace(reader, stencil.State.Front) ||
            !ReadStencilFace(reader, stencil.State.Back) || !reader.U32(stencil.State.ReadMask) ||
            !reader.U32(stencil.State.WriteMask)) {
            return false;
        }
        value.Stencil = stencil;
    }
    return reader.Bool(value.DepthWrite) && reader.Bool(value.DepthClip) && reader.Bool(value.AlphaToMask) &&
           reader.Bool(value.ConservativeRasterization);
}

void WritePass(Writer& writer, const ShaderPassDesc& pass) {
    writer.String(pass.Name);
    writer.String(pass.SourcePath);
    WriteVector(writer, pass.IncludeDirs, [](Writer& target, const string& value) { target.String(value); });
    WriteEnum(writer, pass.SM);
    WriteVector(writer, pass.KeywordGroups, [](Writer& target, const ShaderKeywordGroupDesc& group) {
        WriteVector(target, group.Alternatives, [](Writer& strings, const string& value) { strings.String(value); });
        WriteEnum(target, group.Scope);
        target.U32(group.Stages.value());
    });
    WriteVector(writer, pass.Variants, [](Writer& target, const ShaderVariantDesc& variant) {
        WriteVector(target, variant.Defines, [](Writer& strings, const string& value) { strings.String(value); });
    });
    WriteVector(writer, pass.Tags, [](Writer& target, const ShaderTagDesc& tag) {
        target.String(tag.Name);
        target.String(tag.Value);
    });
    writer.U8(std::holds_alternative<ShaderGraphicsPassDesc>(pass.Program) ? 0 : 1);
    if (const auto* graphics = std::get_if<ShaderGraphicsPassDesc>(&pass.Program)) {
        WriteGraphicsPass(writer, *graphics);
    } else {
        writer.String(std::get<ShaderComputePassDesc>(pass.Program).EntryPoint);
    }
    writer.Bool(pass.IsOptimize);
    writer.Bool(pass.EnableUnbounded);
}

bool ReadPass(Reader& reader, ShaderPassDesc& pass) {
    if (!reader.String(pass.Name) || !reader.String(pass.SourcePath) ||
        !ReadVector(reader, pass.IncludeDirs, [](Reader& source, string& value) { return source.String(value); }) ||
        !ReadEnum(reader, pass.SM, 0, static_cast<int32_t>(HlslShaderModel::SM66)) ||
        !ReadVector(reader, pass.KeywordGroups, [](Reader& source, ShaderKeywordGroupDesc& group) {
            uint32_t stages = 0;
            if (!ReadVector(source, group.Alternatives, [](Reader& strings, string& value) { return strings.String(value); }) ||
                !ReadEnum(source, group.Scope, 0, static_cast<int32_t>(ShaderKeywordScope::Global)) ||
                !source.U32(stages)) {
                return false;
            }
            constexpr uint32_t allStages = static_cast<uint32_t>(ShaderStage::Graphics) |
                                           static_cast<uint32_t>(ShaderStage::Compute) |
                                           static_cast<uint32_t>(ShaderStage::RayTracing);
            if ((stages & ~allStages) != 0) return false;
            group.Stages = ShaderStages{stages};
            return true;
        }) ||
        !ReadVector(reader, pass.Variants, [](Reader& source, ShaderVariantDesc& variant) {
            return ReadVector(source, variant.Defines, [](Reader& strings, string& value) { return strings.String(value); });
        }) ||
        !ReadVector(reader, pass.Tags, [](Reader& source, ShaderTagDesc& tag) {
            return source.String(tag.Name) && source.String(tag.Value);
        })) {
        return false;
    }
    uint8_t kind = 0;
    if (!reader.U8(kind) || kind > 1) return false;
    if (kind == 0) {
        ShaderGraphicsPassDesc graphics;
        if (!ReadGraphicsPass(reader, graphics)) return false;
        pass.Program = std::move(graphics);
    } else {
        ShaderComputePassDesc compute;
        if (!reader.String(compute.EntryPoint)) return false;
        pass.Program = std::move(compute);
    }
    return reader.Bool(pass.IsOptimize) && reader.Bool(pass.EnableUnbounded);
}

std::optional<ShaderReflectionDesc> DeserializeReflection(uint8_t kind, std::string_view payload) noexcept {
    if (kind == 0) {
        auto value = DeserializeHlslShaderDesc(payload);
        if (value.has_value()) return ShaderReflectionDesc{std::move(*value)};
    } else if (kind == 1) {
        auto value = DeserializeSpirvShaderDesc(payload);
        if (value.has_value()) return ShaderReflectionDesc{std::move(*value)};
    }
    return std::nullopt;
}

bool SameRecordKey(const CompiledShaderStage& lhs, const CompiledShaderStage& rhs) noexcept {
    return lhs.Target == rhs.Target && lhs.PassIndex == rhs.PassIndex && lhs.Stage == rhs.Stage && lhs.Defines == rhs.Defines;
}

bool RecordKeyLess(const CompiledShaderStage* lhs, const CompiledShaderStage* rhs) noexcept {
    return std::tie(lhs->PassIndex, lhs->Defines, lhs->Stage, lhs->Target) <
           std::tie(rhs->PassIndex, rhs->Defines, rhs->Stage, rhs->Target);
}

bool IsKnownTarget(ShaderTarget target) noexcept {
    return target == ShaderTarget::DXIL || target == ShaderTarget::SPIRV;
}

size_t GetPassStageCount(const ShaderPassDesc& pass) noexcept {
    if (const auto* graphics = std::get_if<ShaderGraphicsPassDesc>(&pass.Program)) {
        return graphics->PixelEntry.has_value() ? 2u : 1u;
    }
    return 1u;
}

bool IsRecordValid(const ShaderAssetData& asset, const CompiledShaderStage& record) {
    if (!IsKnownTarget(record.Target) || record.PassIndex >= asset.Passes.size() ||
        record.Stage == ShaderStage::UNKNOWN || record.Bytecode.empty() ||
        record.ReflectionPayload.empty() || !record.Reflection.has_value() ||
        record.Category != GetShaderBlobCategory(record.Target) || HashShaderBytes(record.Bytecode) != record.BinaryHash ||
        HashShaderBytes(std::as_bytes(std::span{record.ReflectionPayload.data(), record.ReflectionPayload.size()})) !=
            record.InterfaceHash) {
        return false;
    }

    std::optional<string> serializedReflection;
    if (record.Target == ShaderTarget::DXIL) {
        const auto* reflection = std::get_if<HlslShaderDesc>(&*record.Reflection);
        if (reflection == nullptr) return false;
        serializedReflection = SerializeHlslShaderDesc(*reflection);
    } else {
        const auto* reflection = std::get_if<SpirvShaderDesc>(&*record.Reflection);
        if (reflection == nullptr) return false;
        serializedReflection = SerializeSpirvShaderDesc(*reflection);
    }
    if (!serializedReflection.has_value() || *serializedReflection != record.ReflectionPayload) return false;

    const ShaderPassDesc& pass = asset.Passes[record.PassIndex];
    const auto entry = FindShaderEntryPoint(pass, record.Stage);
    if (!entry.has_value() || *entry != record.EntryPoint || !IsDeclaredShaderVariant(pass, record.Defines)) return false;
    vector<string> normalized = record.Defines;
    NormalizeShaderDefines(normalized);
    return normalized == record.Defines;
}

bool ReplaceWithTemporaryFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) noexcept {
#if defined(RADRAY_PLATFORM_WINDOWS)
    return MoveFileExW(
               temporary.c_str(),
               destination.c_str(),
               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return !error;
#endif
}

}  // namespace

ShaderHash HashShaderBytes(std::span<const byte> data) noexcept {
    const XXH128_hash_t hash = XXH3_128bits(data.data(), data.size());
    return ShaderHash{.Low = hash.low64, .High = hash.high64};
}

ShaderBlobCategory GetShaderBlobCategory(ShaderTarget target) noexcept {
    return target == ShaderTarget::SPIRV ? ShaderBlobCategory::SPIRV : ShaderBlobCategory::DXIL;
}

bool ShaderBinary::IsValid() const noexcept {
    try {
        if (Asset.AssetId.IsEmpty() || Asset.Passes.size() > kMaxPassCount ||
            !IsShaderAssetDataValid(Asset, true) || Stages.empty() ||
            Stages.size() > kMaxRecordCount) {
            return false;
        }

        std::array<bool, 2> targetsPresent{};
        vector<const CompiledShaderStage*> ordered;
        ordered.reserve(Stages.size());
        for (const CompiledShaderStage& stage : Stages) {
            if (!IsRecordValid(Asset, stage)) return false;
            targetsPresent[static_cast<size_t>(stage.Target)] = true;
            ordered.emplace_back(&stage);
        }
        std::ranges::sort(ordered, RecordKeyLess);
        for (size_t i = 1; i < ordered.size(); ++i) {
            if (SameRecordKey(*ordered[i - 1], *ordered[i])) return false;
        }

        size_t recordsPerTarget = 0;
        for (const ShaderPassDesc& pass : Asset.Passes) {
            const size_t stageCount = GetPassStageCount(pass);
            if (pass.Variants.size() > (kMaxRecordCount - recordsPerTarget) / stageCount) return false;
            recordsPerTarget += pass.Variants.size() * stageCount;
        }
        const size_t targetCount = std::ranges::count(targetsPresent, true);
        if (targetCount == 0 || recordsPerTarget > kMaxRecordCount / targetCount) return false;
        return Stages.size() == recordsPerTarget * targetCount;
    } catch (...) {
        return false;
    }
}

Nullable<const CompiledShaderStage*> ShaderBinary::Find(
    ShaderTarget target,
    uint32_t passIndex,
    ShaderStage stage,
    const vector<string>& defines) const noexcept {
    try {
        vector<string> normalized = defines;
        NormalizeShaderDefines(normalized);
        const auto it = std::ranges::find_if(Stages, [&](const CompiledShaderStage& record) noexcept {
            return record.Target == target && record.PassIndex == passIndex && record.Stage == stage &&
                   record.Defines == normalized;
        });
        return it == Stages.end() ? nullptr : &*it;
    } catch (...) {
        return nullptr;
    }
}

bool WriteShaderBinary(const std::filesystem::path& path, const ShaderBinary& binary) noexcept {
    if (!binary.IsValid() || path.empty()) return false;
    try {
        Writer payload;
        WriteGuid(payload, binary.Asset.AssetId);
        WriteVector(payload, binary.Asset.Passes, WritePass);

        vector<const CompiledShaderStage*> ordered;
        ordered.reserve(binary.Stages.size());
        for (const CompiledShaderStage& stage : binary.Stages) ordered.emplace_back(&stage);
        std::ranges::sort(ordered, RecordKeyLess);
        payload.U32(static_cast<uint32_t>(ordered.size()));
        for (const CompiledShaderStage* stage : ordered) {
            payload.U8(static_cast<uint8_t>(stage->Target));
            WriteEnum(payload, stage->Category);
            payload.U32(stage->PassIndex);
            payload.U32(static_cast<uint32_t>(stage->Stage));
            WriteVector(payload, stage->Defines, [](Writer& target, const string& value) { target.String(value); });
            payload.String(stage->EntryPoint);
            WriteHash(payload, stage->BinaryHash);
            WriteHash(payload, stage->InterfaceHash);
            payload.SizedBytes(stage->Bytecode);
            payload.U8(static_cast<uint8_t>(stage->Reflection->index()));
            payload.String(stage->ReflectionPayload);
        }
        if (payload.Data.size() > kMaxFileSize) return false;

        Writer file;
        file.Bytes(std::as_bytes(std::span{kMagic}));
        file.U32(kFormatVersion);
        file.U64(static_cast<uint64_t>(payload.Data.size()));
        WriteHash(file, HashShaderBytes(payload.Data));
        file.Bytes(payload.Data);

        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error) return false;
        }
        std::filesystem::path temporary = path;
        temporary += ".";
        temporary += Guid::NewGuid().ToString();
        temporary += ".tmp";
        if (!WriteBinaryFile(temporary, file.Data)) {
            std::filesystem::remove(temporary, error);
            return false;
        }
        if (ReplaceWithTemporaryFile(temporary, path)) return true;
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    } catch (...) {
        RADRAY_ERR_LOG("failed to serialize shader binary '{}'", path.string());
        return false;
    }
}

std::optional<ShaderBinary> ReadShaderBinary(const std::filesystem::path& path) noexcept {
    try {
        std::error_code sizeError;
        const uintmax_t size = std::filesystem::file_size(path, sizeError);
        if (sizeError || size > kMaxFileSize) return std::nullopt;
        auto data = ReadBinaryFile(path);
        if (!data.has_value()) return std::nullopt;

        Reader file{*data};
        std::span<const byte> magic;
        uint32_t version = 0;
        uint64_t payloadSize = 0;
        ShaderHash expectedHash{};
        if (!file.Bytes(kMagic.size(), magic) || std::memcmp(magic.data(), kMagic.data(), kMagic.size()) != 0 ||
            !file.U32(version) || version != kFormatVersion || !file.U64(payloadSize) || payloadSize > kMaxFileSize ||
            !ReadHash(file, expectedHash) || payloadSize != file.Remaining()) {
            return std::nullopt;
        }
        std::span<const byte> payloadData;
        if (!file.Bytes(static_cast<size_t>(payloadSize), payloadData) || HashShaderBytes(payloadData) != expectedHash ||
            !file.AtEnd()) {
            return std::nullopt;
        }

        Reader payload{payloadData};
        ShaderBinary result;
        if (!ReadGuid(payload, result.Asset.AssetId) ||
            !ReadVector(payload, result.Asset.Passes, ReadPass) || result.Asset.Passes.size() > kMaxPassCount) {
            return std::nullopt;
        }
        uint32_t recordCount = 0;
        if (!payload.U32(recordCount) || recordCount > kMaxRecordCount) return std::nullopt;
        result.Stages.reserve(recordCount);
        for (uint32_t i = 0; i < recordCount; ++i) {
            CompiledShaderStage record;
            uint8_t target = 0;
            uint32_t stage = 0;
            uint8_t reflectionKind = 0;
            if (!payload.U8(target) || target > static_cast<uint8_t>(ShaderTarget::SPIRV) ||
                !ReadEnum(payload, record.Category, 0, static_cast<int32_t>(ShaderBlobCategory::METALLIB)) ||
                !payload.U32(record.PassIndex) || !payload.U32(stage) ||
                !ReadVector(payload, record.Defines, [](Reader& source, string& value) { return source.String(value); }) ||
                !payload.String(record.EntryPoint) || !ReadHash(payload, record.BinaryHash) ||
                !ReadHash(payload, record.InterfaceHash) || !payload.SizedBytes(record.Bytecode) ||
                !payload.U8(reflectionKind) || reflectionKind > 1 || !payload.String(record.ReflectionPayload)) {
                return std::nullopt;
            }
            record.Target = static_cast<ShaderTarget>(target);
            record.Stage = static_cast<ShaderStage>(stage);
            record.Reflection = DeserializeReflection(reflectionKind, record.ReflectionPayload);
            if (!record.Reflection.has_value()) return std::nullopt;
            result.Stages.emplace_back(std::move(record));
        }
        if (!payload.AtEnd() || !result.IsValid()) return std::nullopt;
        return result;
    } catch (...) {
        RADRAY_WARN_LOG("ignoring unreadable shader binary '{}'", path.string());
        return std::nullopt;
    }
}

}  // namespace radray::shader
