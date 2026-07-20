#include <radray/shader/shader_binary.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>

#include <radray/file.h>
#include <radray/logger.h>
#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#endif

namespace radray::shader {
namespace {

constexpr std::array<uint8_t, 8> kMagic{'R', 'D', 'R', 'S', 'H', 'D', 'R', 0};
constexpr uint32_t kFormatVersion = 2;
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
    WriteHash(writer, pass.SourceIdentity);
    WriteVector(writer, pass.IncludeDirs, [](Writer& target, const string& value) { target.String(value); });
    WriteEnum(writer, pass.SM);
    WriteVector(writer, pass.VariantDomain.KeywordGroups, [](Writer& target, const ShaderKeywordGroupDesc& group) {
        WriteVector(target, group.Alternatives, [](Writer& strings, const string& value) { strings.String(value); });
        WriteEnum(target, group.Scope);
        target.U32(group.Stages.value());
    });
    WriteVector(writer, pass.BakeSet.Variants, [](Writer& target, const ShaderVariantKey& variant) {
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
        !ReadHash(reader, pass.SourceIdentity) ||
        !ReadVector(reader, pass.IncludeDirs, [](Reader& source, string& value) { return source.String(value); }) ||
        !ReadEnum(reader, pass.SM, 0, static_cast<int32_t>(HlslShaderModel::SM66)) ||
        !ReadVector(reader, pass.VariantDomain.KeywordGroups, [](Reader& source, ShaderKeywordGroupDesc& group) {
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
        !ReadVector(reader, pass.BakeSet.Variants, [](Reader& source, ShaderVariantKey& variant) {
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

bool IsKnownTarget(ShaderTarget target) noexcept {
    return target == ShaderTarget::DXIL || target == ShaderTarget::SPIRV;
}

bool IsSupportedStage(ShaderStage stage) noexcept {
    return stage == ShaderStage::Vertex || stage == ShaderStage::Pixel || stage == ShaderStage::Compute;
}

vector<ShaderStage> GetPassStages(const ShaderPassDesc& pass) {
    if (const auto* graphics = std::get_if<ShaderGraphicsPassDesc>(&pass.Program)) {
        vector<ShaderStage> result{ShaderStage::Vertex};
        if (graphics->PixelEntry.has_value()) result.emplace_back(ShaderStage::Pixel);
        return result;
    }
    return {ShaderStage::Compute};
}

std::optional<string> SerializeReflection(const ShaderReflectionDesc& reflection) noexcept {
    if (const auto* hlsl = std::get_if<HlslShaderDesc>(&reflection)) {
        return SerializeHlslShaderDesc(*hlsl);
    }
    return SerializeSpirvShaderDesc(std::get<SpirvShaderDesc>(reflection));
}

bool ReflectionMatchesTarget(const ShaderReflectionRecord& record) noexcept {
    return (record.Target == ShaderTarget::DXIL && std::holds_alternative<HlslShaderDesc>(record.Reflection)) ||
           (record.Target == ShaderTarget::SPIRV && std::holds_alternative<SpirvShaderDesc>(record.Reflection));
}

bool IsReflectionValid(const ShaderReflectionRecord& record) noexcept {
    if (!IsKnownTarget(record.Target) || !ReflectionMatchesTarget(record)) return false;
    const auto payload = SerializeReflection(record.Reflection);
    return payload.has_value() &&
           HashShaderBytes(std::as_bytes(std::span{payload->data(), payload->size()})) == record.Hash;
}

bool IsStageInterfaceConsistent(
    const ShaderReflectionRecord& reflection,
    const ShaderStageArtifact& artifact,
    const ShaderStageInterfaceDesc& interface) noexcept {
    ShaderInterfaceNormalizationOptions options;
    options.Context.Target = artifact.Target;
    options.Context.PassIndex = artifact.PassIndex;
    options.Context.VariantDefines = artifact.Defines;
    options.Context.Stage = artifact.Stage;
    if (const auto* hlsl = std::get_if<HlslShaderDesc>(&reflection.Reflection)) {
        const auto result = NormalizeHlslInterface(*hlsl, artifact.Stage, options);
        return result.Succeeded() && *result.Interface == interface;
    }
    const auto result = NormalizeSpirvInterface(
        std::get<SpirvShaderDesc>(reflection.Reflection), artifact.Stage, options);
    return result.Succeeded() && *result.Interface == interface;
}

bool SameStageArtifactKey(const ShaderStageArtifact& lhs, const ShaderStageArtifact& rhs) noexcept {
    return lhs.Target == rhs.Target && lhs.PassIndex == rhs.PassIndex &&
           lhs.Stage == rhs.Stage && lhs.Defines == rhs.Defines;
}

bool StageArtifactKeyLess(const ShaderStageArtifact& lhs, const ShaderStageArtifact& rhs) noexcept {
    return std::tie(lhs.PassIndex, lhs.Defines, lhs.Stage, lhs.Target) <
           std::tie(rhs.PassIndex, rhs.Defines, rhs.Stage, rhs.Target);
}

bool SameProgramKey(
    const ShaderProgramVariantArtifact& lhs,
    const ShaderProgramVariantArtifact& rhs) noexcept {
    return lhs.Target == rhs.Target && lhs.PassIndex == rhs.PassIndex && lhs.Defines == rhs.Defines;
}

bool ProgramKeyLess(
    const ShaderProgramVariantArtifact& lhs,
    const ShaderProgramVariantArtifact& rhs) noexcept {
    return std::tie(lhs.PassIndex, lhs.Defines, lhs.Target) <
           std::tie(rhs.PassIndex, rhs.Defines, rhs.Target);
}

bool BytesLess(std::span<const byte> lhs, std::span<const byte> rhs) noexcept {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

template <typename T, typename Serialize>
bool SortSerializedTable(
    vector<T>& table,
    vector<uint32_t>& oldToNew,
    Serialize&& serialize) {
    vector<vector<byte>> keys;
    keys.reserve(table.size());
    for (const T& value : table) {
        auto key = serialize(value);
        if (!key.has_value()) return false;
        keys.emplace_back(std::move(*key));
    }
    vector<uint32_t> order(table.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::ranges::sort(order, [&](uint32_t lhs, uint32_t rhs) {
        return BytesLess(keys[lhs], keys[rhs]);
    });
    vector<T> sorted;
    sorted.reserve(table.size());
    oldToNew.resize(table.size());
    for (uint32_t newIndex = 0; newIndex < order.size(); ++newIndex) {
        oldToNew[order[newIndex]] = newIndex;
        sorted.emplace_back(std::move(table[order[newIndex]]));
    }
    table = std::move(sorted);
    return true;
}

std::optional<vector<byte>> SerializeReflectionKey(const ShaderReflectionRecord& record) noexcept {
    const auto payload = SerializeReflection(record.Reflection);
    if (!payload.has_value()) return std::nullopt;
    vector<byte> result;
    result.reserve(payload->size() + 1);
    result.emplace_back(static_cast<byte>(record.Target));
    const auto bytes = std::as_bytes(std::span{payload->data(), payload->size()});
    result.insert(result.end(), bytes.begin(), bytes.end());
    return result;
}

bool CanonicalizeBinary(const ShaderBinary& source, ShaderBinary& result) {
    if (!source.IsValid()) return false;
    result = source;

    vector<uint32_t> reflectionRemap;
    if (!SortSerializedTable(
            result.Reflections,
            reflectionRemap,
            [](const ShaderReflectionRecord& value) { return SerializeReflectionKey(value); })) {
        return false;
    }
    vector<uint32_t> stageInterfaceRemap;
    if (!SortSerializedTable(
            result.StageInterfaces,
            stageInterfaceRemap,
            [](const ShaderStageInterfaceDesc& value) { return SerializeShaderStageInterface(value); })) {
        return false;
    }
    vector<uint32_t> programInterfaceRemap;
    if (!SortSerializedTable(
            result.ProgramInterfaces,
            programInterfaceRemap,
            [](const ShaderInterfaceDesc& value) { return SerializeShaderInterface(value); })) {
        return false;
    }

    for (ShaderStageArtifact& artifact : result.StageArtifacts) {
        artifact.ReflectionIndex = reflectionRemap[artifact.ReflectionIndex];
        artifact.InterfaceIndex = stageInterfaceRemap[artifact.InterfaceIndex];
    }
    vector<uint32_t> stageArtifactOrder(result.StageArtifacts.size());
    for (uint32_t i = 0; i < stageArtifactOrder.size(); ++i) stageArtifactOrder[i] = i;
    std::ranges::sort(stageArtifactOrder, [&](uint32_t lhs, uint32_t rhs) {
        return StageArtifactKeyLess(result.StageArtifacts[lhs], result.StageArtifacts[rhs]);
    });
    vector<uint32_t> stageArtifactRemap(result.StageArtifacts.size());
    vector<ShaderStageArtifact> sortedArtifacts;
    sortedArtifacts.reserve(result.StageArtifacts.size());
    for (uint32_t newIndex = 0; newIndex < stageArtifactOrder.size(); ++newIndex) {
        stageArtifactRemap[stageArtifactOrder[newIndex]] = newIndex;
        sortedArtifacts.emplace_back(std::move(result.StageArtifacts[stageArtifactOrder[newIndex]]));
    }
    result.StageArtifacts = std::move(sortedArtifacts);

    for (ShaderProgramVariantArtifact& program : result.ProgramVariants) {
        program.InterfaceIndex = programInterfaceRemap[program.InterfaceIndex];
        for (uint32_t& artifactIndex : program.StageArtifactIndices) {
            artifactIndex = stageArtifactRemap[artifactIndex];
        }
    }
    std::ranges::sort(result.ProgramVariants, ProgramKeyLess);
    return result.IsValid();
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

ShaderBlobCategory GetShaderBlobCategory(ShaderTarget target) noexcept {
    return target == ShaderTarget::SPIRV ? ShaderBlobCategory::SPIRV : ShaderBlobCategory::DXIL;
}

bool ShaderBinary::IsValid() const noexcept {
    try {
        if (Asset.AssetId.IsEmpty() || Asset.Passes.size() > kMaxPassCount ||
            !IsShaderAssetDataValid(Asset, false) || Reflections.size() > kMaxRecordCount ||
            StageInterfaces.size() > kMaxRecordCount || ProgramInterfaces.size() > kMaxRecordCount ||
            StageArtifacts.size() > kMaxRecordCount || ProgramVariants.size() > kMaxRecordCount) {
            return false;
        }

        vector<string> reflectionPayloads;
        reflectionPayloads.reserve(Reflections.size());
        for (size_t i = 0; i < Reflections.size(); ++i) {
            const ShaderReflectionRecord& reflection = Reflections[i];
            if (!IsReflectionValid(reflection)) return false;
            auto payload = SerializeReflection(reflection.Reflection);
            if (!payload.has_value()) return false;
            for (size_t j = 0; j < i; ++j) {
                if (Reflections[j].Target == reflection.Target && reflectionPayloads[j] == *payload) {
                    return false;
                }
            }
            reflectionPayloads.emplace_back(std::move(*payload));
        }
        for (size_t i = 0; i < StageInterfaces.size(); ++i) {
            if (!IsShaderStageInterfaceValid(StageInterfaces[i])) return false;
            for (size_t j = 0; j < i; ++j) {
                if (StageInterfaces[j] == StageInterfaces[i]) return false;
            }
        }
        for (size_t i = 0; i < ProgramInterfaces.size(); ++i) {
            if (!IsShaderInterfaceValid(ProgramInterfaces[i])) return false;
            for (size_t j = 0; j < i; ++j) {
                if (ProgramInterfaces[j] == ProgramInterfaces[i]) return false;
            }
        }

        vector<bool> reflectionReferenced(Reflections.size(), false);
        vector<bool> stageInterfaceReferenced(StageInterfaces.size(), false);
        for (size_t i = 0; i < StageArtifacts.size(); ++i) {
            const ShaderStageArtifact& artifact = StageArtifacts[i];
            if (!IsKnownTarget(artifact.Target) || artifact.Category != GetShaderBlobCategory(artifact.Target) ||
                artifact.PassIndex >= Asset.Passes.size() || !IsSupportedStage(artifact.Stage) ||
                artifact.Bytecode.empty() || HashShaderBytes(artifact.Bytecode) != artifact.BinaryHash ||
                artifact.ReflectionIndex >= Reflections.size() || artifact.InterfaceIndex >= StageInterfaces.size()) {
                return false;
            }
            const ShaderPassDesc& pass = Asset.Passes[artifact.PassIndex];
            const vector<ShaderStage> passStages = GetPassStages(pass);
            if (std::ranges::find(passStages, artifact.Stage) == passStages.end()) return false;
            const auto entry = FindShaderEntryPoint(pass, artifact.Stage);
            if (!entry.has_value() || artifact.EntryPoint != *entry) return false;
            vector<string> normalized = artifact.Defines;
            NormalizeShaderDefines(normalized);
            if (normalized != artifact.Defines ||
                !AreShaderDefinesValid(pass, artifact.Stage, artifact.Defines)) {
                return false;
            }
            const ShaderReflectionRecord& reflection = Reflections[artifact.ReflectionIndex];
            const ShaderStageInterfaceDesc& interface = StageInterfaces[artifact.InterfaceIndex];
            if (reflection.Target != artifact.Target || interface.Stage != artifact.Stage ||
                !IsStageInterfaceConsistent(reflection, artifact, interface)) {
                return false;
            }
            for (size_t j = 0; j < i; ++j) {
                if (SameStageArtifactKey(StageArtifacts[j], artifact)) return false;
            }
            reflectionReferenced[artifact.ReflectionIndex] = true;
            stageInterfaceReferenced[artifact.InterfaceIndex] = true;
        }

        vector<bool> stageArtifactReferenced(StageArtifacts.size(), false);
        vector<bool> programInterfaceReferenced(ProgramInterfaces.size(), false);
        for (size_t i = 0; i < ProgramVariants.size(); ++i) {
            const ShaderProgramVariantArtifact& program = ProgramVariants[i];
            if (!IsKnownTarget(program.Target) || program.PassIndex >= Asset.Passes.size() ||
                program.InterfaceIndex >= ProgramInterfaces.size()) {
                return false;
            }
            const ShaderPassDesc& pass = Asset.Passes[program.PassIndex];
            vector<string> normalized = program.Defines;
            NormalizeShaderDefines(normalized);
            if (normalized != program.Defines || !IsBakedShaderVariant(pass, program.Defines)) return false;
            const vector<ShaderStage> expectedStages = GetPassStages(pass);
            if (program.StageArtifactIndices.size() != expectedStages.size()) return false;

            vector<const ShaderStageInterfaceDesc*> stageInterfaces;
            stageInterfaces.reserve(expectedStages.size());
            for (size_t stageIndex = 0; stageIndex < expectedStages.size(); ++stageIndex) {
                const uint32_t artifactIndex = program.StageArtifactIndices[stageIndex];
                if (artifactIndex >= StageArtifacts.size()) return false;
                const ShaderStageArtifact& artifact = StageArtifacts[artifactIndex];
                if (artifact.Target != program.Target || artifact.PassIndex != program.PassIndex ||
                    artifact.Stage != expectedStages[stageIndex] ||
                    artifact.Defines != ProjectShaderDefines(pass, artifact.Stage, program.Defines)) {
                    return false;
                }
                stageArtifactReferenced[artifactIndex] = true;
                stageInterfaces.emplace_back(&StageInterfaces[artifact.InterfaceIndex]);
            }

            ShaderInterfaceBuildResult built;
            if (std::holds_alternative<ShaderGraphicsPassDesc>(pass.Program)) {
                built = stageInterfaces.size() == 1
                            ? MergeGraphicsStageInterfaces(*stageInterfaces[0])
                            : MergeGraphicsStageInterfaces(*stageInterfaces[0], *stageInterfaces[1]);
            } else {
                built = BuildComputeShaderInterface(*stageInterfaces[0]);
            }
            if (!built.Succeeded() || *built.Interface != ProgramInterfaces[program.InterfaceIndex]) {
                return false;
            }
            for (size_t j = 0; j < i; ++j) {
                if (SameProgramKey(ProgramVariants[j], program)) return false;
            }
            programInterfaceReferenced[program.InterfaceIndex] = true;
        }

        if (!std::ranges::all_of(reflectionReferenced, [](bool value) { return value; }) ||
            !std::ranges::all_of(stageInterfaceReferenced, [](bool value) { return value; }) ||
            !std::ranges::all_of(stageArtifactReferenced, [](bool value) { return value; }) ||
            !std::ranges::all_of(programInterfaceReferenced, [](bool value) { return value; })) {
            return false;
        }

        for (size_t i = 0; i < StageArtifacts.size(); ++i) {
            for (size_t j = 0; j < i; ++j) {
                const ShaderStageArtifact& lhs = StageArtifacts[i];
                const ShaderStageArtifact& rhs = StageArtifacts[j];
                if (lhs.Target != rhs.Target && lhs.PassIndex == rhs.PassIndex &&
                    lhs.Stage == rhs.Stage && lhs.Defines == rhs.Defines &&
                    StageInterfaces[lhs.InterfaceIndex] != StageInterfaces[rhs.InterfaceIndex]) {
                    return false;
                }
            }
        }
        for (size_t i = 0; i < ProgramVariants.size(); ++i) {
            for (size_t j = 0; j < i; ++j) {
                const ShaderProgramVariantArtifact& lhs = ProgramVariants[i];
                const ShaderProgramVariantArtifact& rhs = ProgramVariants[j];
                if (lhs.Target != rhs.Target && lhs.PassIndex == rhs.PassIndex && lhs.Defines == rhs.Defines &&
                    ProgramInterfaces[lhs.InterfaceIndex] != ProgramInterfaces[rhs.InterfaceIndex]) {
                    return false;
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ShaderBinary::IsBakeComplete(ShaderTarget target) const noexcept {
    if (!IsKnownTarget(target) || !IsValid()) return false;
    try {
        for (uint32_t passIndex = 0; passIndex < Asset.Passes.size(); ++passIndex) {
            for (const ShaderVariantKey& variant : Asset.Passes[passIndex].BakeSet.Variants) {
                if (!FindProgramVariant(target, passIndex, variant.Defines).HasValue()) return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

Nullable<const ShaderStageArtifact*> ShaderBinary::FindStageArtifact(
    ShaderTarget target,
    uint32_t passIndex,
    ShaderStage stage,
    const vector<string>& fullDefines) const noexcept {
    try {
        if (!IsKnownTarget(target) || passIndex >= Asset.Passes.size() || !IsSupportedStage(stage)) return nullptr;
        vector<string> normalized = fullDefines;
        NormalizeShaderDefines(normalized);
        if (!IsShaderVariantInDomain(Asset.Passes[passIndex], normalized)) return nullptr;
        const vector<string> projected = ProjectShaderDefines(Asset.Passes[passIndex], stage, normalized);
        const auto it = std::ranges::find_if(StageArtifacts, [&](const ShaderStageArtifact& artifact) noexcept {
            return artifact.Target == target && artifact.PassIndex == passIndex && artifact.Stage == stage &&
                   artifact.Defines == projected;
        });
        return it == StageArtifacts.end() ? nullptr : &*it;
    } catch (...) {
        return nullptr;
    }
}

Nullable<const ShaderProgramVariantArtifact*> ShaderBinary::FindProgramVariant(
    ShaderTarget target,
    uint32_t passIndex,
    const vector<string>& fullDefines) const noexcept {
    try {
        if (!IsKnownTarget(target) || passIndex >= Asset.Passes.size()) return nullptr;
        vector<string> normalized = fullDefines;
        NormalizeShaderDefines(normalized);
        if (!IsShaderVariantInDomain(Asset.Passes[passIndex], normalized)) return nullptr;
        const auto it = std::ranges::find_if(
            ProgramVariants,
            [&](const ShaderProgramVariantArtifact& program) noexcept {
                return program.Target == target && program.PassIndex == passIndex &&
                       program.Defines == normalized;
            });
        return it == ProgramVariants.end() ? nullptr : &*it;
    } catch (...) {
        return nullptr;
    }
}

Nullable<const ShaderReflectionRecord*> ShaderBinary::GetReflection(
    const ShaderStageArtifact& artifact) const noexcept {
    return artifact.ReflectionIndex < Reflections.size() ? &Reflections[artifact.ReflectionIndex] : nullptr;
}

Nullable<const ShaderStageInterfaceDesc*> ShaderBinary::GetStageInterface(
    const ShaderStageArtifact& artifact) const noexcept {
    return artifact.InterfaceIndex < StageInterfaces.size() ? &StageInterfaces[artifact.InterfaceIndex] : nullptr;
}

Nullable<const ShaderInterfaceDesc*> ShaderBinary::GetProgramInterface(
    const ShaderProgramVariantArtifact& program) const noexcept {
    return program.InterfaceIndex < ProgramInterfaces.size() ? &ProgramInterfaces[program.InterfaceIndex] : nullptr;
}

namespace {

std::optional<vector<byte>> SerializeShaderBinaryBytes(const ShaderBinary& binary) {
    ShaderBinary canonical;
    if (!CanonicalizeBinary(binary, canonical)) return std::nullopt;

    Writer payload;
    WriteGuid(payload, canonical.Asset.AssetId);
    WriteVector(payload, canonical.Asset.Passes, WritePass);
    WriteVector(payload, canonical.Reflections, [](Writer& writer, const ShaderReflectionRecord& reflection) {
        writer.U8(static_cast<uint8_t>(reflection.Target));
        WriteHash(writer, reflection.Hash);
        const auto serialized = SerializeReflection(reflection.Reflection);
        if (!serialized.has_value()) throw std::runtime_error{"reflection serialization failed"};
        writer.String(*serialized);
    });
    WriteVector(payload, canonical.StageInterfaces, [](Writer& writer, const ShaderStageInterfaceDesc& interface) {
        const auto serialized = SerializeShaderStageInterface(interface);
        if (!serialized.has_value()) throw std::runtime_error{"stage interface serialization failed"};
        writer.SizedBytes(*serialized);
    });
    WriteVector(payload, canonical.ProgramInterfaces, [](Writer& writer, const ShaderInterfaceDesc& interface) {
        const auto serialized = SerializeShaderInterface(interface);
        if (!serialized.has_value()) throw std::runtime_error{"program interface serialization failed"};
        writer.SizedBytes(*serialized);
    });
    WriteVector(payload, canonical.StageArtifacts, [](Writer& writer, const ShaderStageArtifact& artifact) {
        writer.U8(static_cast<uint8_t>(artifact.Target));
        WriteEnum(writer, artifact.Category);
        writer.U32(artifact.PassIndex);
        writer.U32(static_cast<uint32_t>(artifact.Stage));
        WriteVector(writer, artifact.Defines, [](Writer& strings, const string& value) { strings.String(value); });
        writer.String(artifact.EntryPoint);
        WriteHash(writer, artifact.BinaryHash);
        writer.U32(artifact.ReflectionIndex);
        writer.U32(artifact.InterfaceIndex);
        writer.SizedBytes(artifact.Bytecode);
    });
    WriteVector(payload, canonical.ProgramVariants, [](Writer& writer, const ShaderProgramVariantArtifact& program) {
        writer.U8(static_cast<uint8_t>(program.Target));
        writer.U32(program.PassIndex);
        WriteVector(writer, program.Defines, [](Writer& strings, const string& value) { strings.String(value); });
        WriteVector(writer, program.StageArtifactIndices, [](Writer& indices, uint32_t value) { indices.U32(value); });
        writer.U32(program.InterfaceIndex);
    });
    if (payload.Data.size() > kMaxFileSize) return std::nullopt;

    Writer file;
    file.Bytes(std::as_bytes(std::span{kMagic}));
    file.U32(kFormatVersion);
    file.U64(static_cast<uint64_t>(payload.Data.size()));
    WriteHash(file, HashShaderBytes(payload.Data));
    file.Bytes(payload.Data);
    return std::move(file.Data);
}

}  // namespace

bool WriteShaderBinary(const std::filesystem::path& path, const ShaderBinary& binary) noexcept {
    if (path.empty()) return false;
    try {
        const auto data = SerializeShaderBinaryBytes(binary);
        if (!data.has_value()) return false;
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
        if (!WriteBinaryFile(temporary, *data)) {
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
        if (!ReadVector(payload, result.Reflections, [](Reader& reader, ShaderReflectionRecord& reflection) {
                uint8_t target = 0;
                string serialized;
                if (!reader.U8(target) || target > static_cast<uint8_t>(ShaderTarget::SPIRV) ||
                    !ReadHash(reader, reflection.Hash) || !reader.String(serialized)) {
                    return false;
                }
                reflection.Target = static_cast<ShaderTarget>(target);
                auto value = DeserializeReflection(target == 0 ? 0 : 1, serialized);
                if (!value.has_value()) return false;
                reflection.Reflection = std::move(*value);
                return true;
            }) ||
            result.Reflections.size() > kMaxRecordCount || !ReadVector(payload, result.StageInterfaces, [](Reader& reader, ShaderStageInterfaceDesc& interface) {
                vector<byte> serialized;
                if (!reader.SizedBytes(serialized)) return false;
                auto value = DeserializeShaderStageInterface(serialized);
                if (!value.has_value()) return false;
                interface = std::move(*value);
                return true;
            }) ||
            result.StageInterfaces.size() > kMaxRecordCount || !ReadVector(payload, result.ProgramInterfaces, [](Reader& reader, ShaderInterfaceDesc& interface) {
                vector<byte> serialized;
                if (!reader.SizedBytes(serialized)) return false;
                auto value = DeserializeShaderInterface(serialized);
                if (!value.has_value()) return false;
                interface = std::move(*value);
                return true;
            }) ||
            result.ProgramInterfaces.size() > kMaxRecordCount || !ReadVector(payload, result.StageArtifacts, [](Reader& reader, ShaderStageArtifact& artifact) {
                uint8_t target = 0;
                uint32_t stage = 0;
                if (!reader.U8(target) || target > static_cast<uint8_t>(ShaderTarget::SPIRV) ||
                    !ReadEnum(reader, artifact.Category, 0, static_cast<int32_t>(ShaderBlobCategory::METALLIB)) ||
                    !reader.U32(artifact.PassIndex) || !reader.U32(stage) ||
                    !ReadVector(reader, artifact.Defines, [](Reader& strings, string& value) {
                        return strings.String(value);
                    }) ||
                    !reader.String(artifact.EntryPoint) || !ReadHash(reader, artifact.BinaryHash) || !reader.U32(artifact.ReflectionIndex) || !reader.U32(artifact.InterfaceIndex) || !reader.SizedBytes(artifact.Bytecode)) {
                    return false;
                }
                artifact.Target = static_cast<ShaderTarget>(target);
                artifact.Stage = static_cast<ShaderStage>(stage);
                return IsSupportedStage(artifact.Stage);
            }) ||
            result.StageArtifacts.size() > kMaxRecordCount || !ReadVector(payload, result.ProgramVariants, [](Reader& reader, ShaderProgramVariantArtifact& program) {
                uint8_t target = 0;
                if (!reader.U8(target) || target > static_cast<uint8_t>(ShaderTarget::SPIRV) ||
                    !reader.U32(program.PassIndex) ||
                    !ReadVector(reader, program.Defines, [](Reader& strings, string& value) {
                        return strings.String(value);
                    }) ||
                    !ReadVector(reader, program.StageArtifactIndices, [](Reader& indices, uint32_t& value) {
                        return indices.U32(value);
                    }) ||
                    !reader.U32(program.InterfaceIndex)) {
                    return false;
                }
                program.Target = static_cast<ShaderTarget>(target);
                return true;
            }) ||
            result.ProgramVariants.size() > kMaxRecordCount) {
            return std::nullopt;
        }
        if (!payload.AtEnd() || !result.IsValid()) return std::nullopt;
        const auto canonical = SerializeShaderBinaryBytes(result);
        if (!canonical.has_value() || *canonical != *data) return std::nullopt;
        return result;
    } catch (...) {
        RADRAY_WARN_LOG("ignoring unreadable shader binary '{}'", path.string());
        return std::nullopt;
    }
}

}  // namespace radray::shader
