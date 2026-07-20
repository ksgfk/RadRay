#include <radray/shader/shader_interface.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <limits>
#include <tuple>
#include <type_traits>

#include <fmt/format.h>

namespace radray::shader {
namespace {

constexpr bool IsSupportedStage(ShaderStage stage) noexcept {
    return stage == ShaderStage::Vertex || stage == ShaderStage::Pixel || stage == ShaderStage::Compute;
}

template <typename Enum>
constexpr bool IsEnumInRange(Enum value, Enum maximum) noexcept {
    using Underlying = std::underlying_type_t<Enum>;
    return static_cast<Underlying>(value) <= static_cast<Underlying>(maximum);
}

constexpr uint32_t kSupportedStageMask =
    static_cast<uint32_t>(ShaderStage::Vertex) |
    static_cast<uint32_t>(ShaderStage::Pixel) |
    static_cast<uint32_t>(ShaderStage::Compute);

bool AreShaderStagesValid(ShaderStages stages) noexcept {
    return stages.value() != 0 && (stages.value() & ~kSupportedStageMask) == 0;
}

void AddDiagnostic(
    vector<ShaderDiagnostic>& diagnostics,
    const ShaderInterfaceNormalizationOptions& options,
    ShaderStage stage,
    ShaderDiagnosticCode code,
    string message,
    std::optional<uint32_t> group = {},
    std::optional<uint32_t> binding = {}) {
    ShaderDiagnosticContext context = options.Context;
    context.Stage = stage;
    context.Group = group;
    context.Binding = binding;
    diagnostics.emplace_back(ShaderDiagnostic{
        .Code = code,
        .Message = std::move(message),
        .Context = std::move(context)});
}

void AddMergeDiagnostic(
    vector<ShaderDiagnostic>& diagnostics,
    const ShaderDiagnosticContext& sourceContext,
    ShaderStage stage,
    ShaderDiagnosticCode code,
    string message,
    std::optional<uint32_t> group = {},
    std::optional<uint32_t> binding = {}) {
    ShaderDiagnosticContext context = sourceContext;
    context.Stage = stage;
    context.Group = group;
    context.Binding = binding;
    diagnostics.emplace_back(ShaderDiagnostic{
        .Code = code,
        .Message = std::move(message),
        .Context = std::move(context)});
}

ShaderScalarType MapHlslScalar(HlslShaderVariableType type) noexcept {
    switch (type) {
        case HlslShaderVariableType::BOOL: return ShaderScalarType::Bool;
        case HlslShaderVariableType::UINT8: return ShaderScalarType::UInt8;
        case HlslShaderVariableType::MIN12INT:
        case HlslShaderVariableType::MIN16INT:
        case HlslShaderVariableType::INT16: return ShaderScalarType::Int16;
        case HlslShaderVariableType::MIN16UINT:
        case HlslShaderVariableType::UINT16: return ShaderScalarType::UInt16;
        case HlslShaderVariableType::INT: return ShaderScalarType::Int32;
        case HlslShaderVariableType::UINT: return ShaderScalarType::UInt32;
        case HlslShaderVariableType::INT64: return ShaderScalarType::Int64;
        case HlslShaderVariableType::UINT64: return ShaderScalarType::UInt64;
        case HlslShaderVariableType::MIN8FLOAT:
        case HlslShaderVariableType::MIN10FLOAT:
        case HlslShaderVariableType::MIN16FLOAT:
        case HlslShaderVariableType::FLOAT16: return ShaderScalarType::Float16;
        case HlslShaderVariableType::FLOAT: return ShaderScalarType::Float32;
        case HlslShaderVariableType::DOUBLE: return ShaderScalarType::Float64;
        default: return ShaderScalarType::Unknown;
    }
}

ShaderScalarType MapHlslIoScalar(HlslRegisterComponentType type) noexcept {
    switch (type) {
        case HlslRegisterComponentType::UINT16: return ShaderScalarType::UInt16;
        case HlslRegisterComponentType::SINT16: return ShaderScalarType::Int16;
        case HlslRegisterComponentType::FLOAT16: return ShaderScalarType::Float16;
        case HlslRegisterComponentType::UINT32: return ShaderScalarType::UInt32;
        case HlslRegisterComponentType::SINT32: return ShaderScalarType::Int32;
        case HlslRegisterComponentType::FLOAT32: return ShaderScalarType::Float32;
        case HlslRegisterComponentType::UINT64: return ShaderScalarType::UInt64;
        case HlslRegisterComponentType::SINT64: return ShaderScalarType::Int64;
        case HlslRegisterComponentType::FLOAT64: return ShaderScalarType::Float64;
        default: return ShaderScalarType::Unknown;
    }
}

ShaderScalarType MapSpirvScalar(SpirvBaseType type) noexcept {
    switch (type) {
        case SpirvBaseType::Bool: return ShaderScalarType::Bool;
        case SpirvBaseType::Int8: return ShaderScalarType::Int8;
        case SpirvBaseType::UInt8: return ShaderScalarType::UInt8;
        case SpirvBaseType::Int16: return ShaderScalarType::Int16;
        case SpirvBaseType::UInt16: return ShaderScalarType::UInt16;
        case SpirvBaseType::Int32: return ShaderScalarType::Int32;
        case SpirvBaseType::UInt32: return ShaderScalarType::UInt32;
        case SpirvBaseType::Int64: return ShaderScalarType::Int64;
        case SpirvBaseType::UInt64: return ShaderScalarType::UInt64;
        case SpirvBaseType::Float16: return ShaderScalarType::Float16;
        case SpirvBaseType::Float32: return ShaderScalarType::Float32;
        case SpirvBaseType::Float64: return ShaderScalarType::Float64;
        default: return ShaderScalarType::Unknown;
    }
}

uint32_t ScalarByteSize(ShaderScalarType type) noexcept {
    switch (type) {
        case ShaderScalarType::Int8:
        case ShaderScalarType::UInt8: return 1;
        case ShaderScalarType::Int16:
        case ShaderScalarType::UInt16:
        case ShaderScalarType::Float16: return 2;
        case ShaderScalarType::Bool:
        case ShaderScalarType::Int32:
        case ShaderScalarType::UInt32:
        case ShaderScalarType::Float32: return 4;
        case ShaderScalarType::Int64:
        case ShaderScalarType::UInt64:
        case ShaderScalarType::Float64: return 8;
        default: return 0;
    }
}

ShaderSampleType MapHlslSampleType(HlslResourceReturnType type) noexcept {
    switch (type) {
        case HlslResourceReturnType::UNORM: return ShaderSampleType::UNorm;
        case HlslResourceReturnType::SNORM: return ShaderSampleType::SNorm;
        case HlslResourceReturnType::SINT: return ShaderSampleType::SInt;
        case HlslResourceReturnType::UINT: return ShaderSampleType::UInt;
        case HlslResourceReturnType::FLOAT:
        case HlslResourceReturnType::DOUBLE: return ShaderSampleType::Float;
        default: return ShaderSampleType::Unknown;
    }
}

ShaderSampleType MapSpirvSampleType(const SpirvShaderDesc& reflection, const SpirvImageInfo& image) noexcept {
    if (image.Depth) return ShaderSampleType::Depth;
    if (image.SampledType >= reflection.Types.size()) return ShaderSampleType::Unknown;
    switch (reflection.Types[image.SampledType].BaseType) {
        case SpirvBaseType::Int8:
        case SpirvBaseType::Int16:
        case SpirvBaseType::Int32:
        case SpirvBaseType::Int64: return ShaderSampleType::SInt;
        case SpirvBaseType::UInt8:
        case SpirvBaseType::UInt16:
        case SpirvBaseType::UInt32:
        case SpirvBaseType::UInt64: return ShaderSampleType::UInt;
        case SpirvBaseType::Float16:
        case SpirvBaseType::Float32:
        case SpirvBaseType::Float64: return ShaderSampleType::Float;
        default: return ShaderSampleType::Unknown;
    }
}

ShaderTextureDimension MapHlslDimension(HlslSRVDimension dimension) noexcept {
    switch (dimension) {
        case HlslSRVDimension::TEXTURE1D:
        case HlslSRVDimension::TEXTURE1DARRAY: return ShaderTextureDimension::Dim1D;
        case HlslSRVDimension::TEXTURE2D:
        case HlslSRVDimension::TEXTURE2DARRAY:
        case HlslSRVDimension::TEXTURE2DMS:
        case HlslSRVDimension::TEXTURE2DMSARRAY: return ShaderTextureDimension::Dim2D;
        case HlslSRVDimension::TEXTURE3D: return ShaderTextureDimension::Dim3D;
        case HlslSRVDimension::TEXTURECUBE:
        case HlslSRVDimension::TEXTURECUBEARRAY: return ShaderTextureDimension::Cube;
        case HlslSRVDimension::BUFFER:
        case HlslSRVDimension::BUFFEREX: return ShaderTextureDimension::Buffer;
        default: return ShaderTextureDimension::Unknown;
    }
}

bool IsHlslArrayDimension(HlslSRVDimension dimension) noexcept {
    switch (dimension) {
        case HlslSRVDimension::TEXTURE1DARRAY:
        case HlslSRVDimension::TEXTURE2DARRAY:
        case HlslSRVDimension::TEXTURE2DMSARRAY:
        case HlslSRVDimension::TEXTURECUBEARRAY: return true;
        default: return false;
    }
}

bool IsHlslMultisampled(HlslSRVDimension dimension) noexcept {
    return dimension == HlslSRVDimension::TEXTURE2DMS ||
           dimension == HlslSRVDimension::TEXTURE2DMSARRAY;
}

ShaderTextureDimension MapSpirvDimension(SpirvImageDim dimension) noexcept {
    switch (dimension) {
        case SpirvImageDim::Dim1D: return ShaderTextureDimension::Dim1D;
        case SpirvImageDim::Dim2D: return ShaderTextureDimension::Dim2D;
        case SpirvImageDim::Dim3D: return ShaderTextureDimension::Dim3D;
        case SpirvImageDim::Cube: return ShaderTextureDimension::Cube;
        case SpirvImageDim::Buffer: return ShaderTextureDimension::Buffer;
        default: return ShaderTextureDimension::Unknown;
    }
}

string UpperAscii(std::string_view value) {
    string result{value};
    for (char& c : result) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return result;
}

struct ParsedSemantic {
    string Name;
    uint32_t Index{0};
};

ParsedSemantic ParseSemantic(std::string_view value, uint32_t fallbackIndex) {
    ParsedSemantic result{.Name = UpperAscii(value), .Index = fallbackIndex};
    const size_t separator = result.Name.rfind('.');
    if (separator != string::npos) {
        result.Name.erase(0, separator + 1);
    }
    size_t digits = result.Name.size();
    while (digits > 0 && result.Name[digits - 1] >= '0' && result.Name[digits - 1] <= '9') {
        --digits;
    }
    if (digits != result.Name.size()) {
        uint64_t index = 0;
        for (size_t i = digits; i < result.Name.size(); ++i) {
            index = index * 10 + static_cast<uint32_t>(result.Name[i] - '0');
        }
        if (index <= std::numeric_limits<uint32_t>::max()) {
            result.Index = static_cast<uint32_t>(index);
            result.Name.resize(digits);
        }
    }
    return result;
}

ShaderBuiltin MapSemanticBuiltin(std::string_view semantic) noexcept {
    if (semantic == "SV_POSITION") return ShaderBuiltin::Position;
    if (semantic == "SV_CLIPDISTANCE") return ShaderBuiltin::ClipDistance;
    if (semantic == "SV_CULLDISTANCE") return ShaderBuiltin::CullDistance;
    if (semantic == "SV_RENDERTARGETARRAYINDEX") return ShaderBuiltin::RenderTargetArrayIndex;
    if (semantic == "SV_VIEWPORTARRAYINDEX") return ShaderBuiltin::ViewportArrayIndex;
    if (semantic == "SV_VERTEXID") return ShaderBuiltin::VertexIndex;
    if (semantic == "SV_PRIMITIVEID") return ShaderBuiltin::PrimitiveIndex;
    if (semantic == "SV_INSTANCEID") return ShaderBuiltin::InstanceIndex;
    if (semantic == "SV_ISFRONTFACE") return ShaderBuiltin::FrontFacing;
    if (semantic == "SV_SAMPLEINDEX") return ShaderBuiltin::SampleIndex;
    if (semantic == "SV_DEPTH" || semantic == "SV_DEPTHGREATEREQUAL" || semantic == "SV_DEPTHLESSEQUAL") {
        return ShaderBuiltin::FragDepth;
    }
    if (semantic == "SV_COVERAGE" || semantic == "SV_INNERCOVERAGE") return ShaderBuiltin::Coverage;
    if (semantic == "SV_STENCILREF") return ShaderBuiltin::StencilRef;
    return ShaderBuiltin::None;
}

ShaderBuiltin MapHlslBuiltin(HlslSystemValueType value, std::string_view semantic) noexcept {
    switch (value) {
        case HlslSystemValueType::POSITION: return ShaderBuiltin::Position;
        case HlslSystemValueType::CLIP_DISTANCE: return ShaderBuiltin::ClipDistance;
        case HlslSystemValueType::CULL_DISTANCE: return ShaderBuiltin::CullDistance;
        case HlslSystemValueType::RENDER_TARGET_ARRAY_INDEX: return ShaderBuiltin::RenderTargetArrayIndex;
        case HlslSystemValueType::VIEWPORT_ARRAY_INDEX: return ShaderBuiltin::ViewportArrayIndex;
        case HlslSystemValueType::VERTEX_ID: return ShaderBuiltin::VertexIndex;
        case HlslSystemValueType::PRIMITIVE_ID: return ShaderBuiltin::PrimitiveIndex;
        case HlslSystemValueType::INSTANCE_ID: return ShaderBuiltin::InstanceIndex;
        case HlslSystemValueType::IS_FRONT_FACE: return ShaderBuiltin::FrontFacing;
        case HlslSystemValueType::SAMPLE_INDEX: return ShaderBuiltin::SampleIndex;
        case HlslSystemValueType::DEPTH:
        case HlslSystemValueType::DEPTH_GREATER_EQUAL:
        case HlslSystemValueType::DEPTH_LESS_EQUAL: return ShaderBuiltin::FragDepth;
        case HlslSystemValueType::COVERAGE:
        case HlslSystemValueType::INNER_COVERAGE: return ShaderBuiltin::Coverage;
        case HlslSystemValueType::STENCIL_REF: return ShaderBuiltin::StencilRef;
        default: return MapSemanticBuiltin(semantic);
    }
}

ShaderBuiltin MapSpirvBuiltin(uint32_t value) noexcept {
    switch (value) {
        case 0: return ShaderBuiltin::Position;
        case 3: return ShaderBuiltin::ClipDistance;
        case 4: return ShaderBuiltin::CullDistance;
        case 5: return ShaderBuiltin::VertexIndex;
        case 6: return ShaderBuiltin::InstanceIndex;
        case 7: return ShaderBuiltin::PrimitiveIndex;
        case 9: return ShaderBuiltin::RenderTargetArrayIndex;
        case 10: return ShaderBuiltin::ViewportArrayIndex;
        case 15: return ShaderBuiltin::Position;
        case 17: return ShaderBuiltin::FrontFacing;
        case 18: return ShaderBuiltin::SampleIndex;
        case 22: return ShaderBuiltin::FragDepth;
        case 23: return ShaderBuiltin::Coverage;
        case 42: return ShaderBuiltin::VertexIndex;
        case 43: return ShaderBuiltin::InstanceIndex;
        default: return ShaderBuiltin::None;
    }
}

std::string_view BuiltinSemantic(ShaderBuiltin builtin) noexcept {
    switch (builtin) {
        case ShaderBuiltin::Position: return "SV_POSITION";
        case ShaderBuiltin::ClipDistance: return "SV_CLIPDISTANCE";
        case ShaderBuiltin::CullDistance: return "SV_CULLDISTANCE";
        case ShaderBuiltin::RenderTargetArrayIndex: return "SV_RENDERTARGETARRAYINDEX";
        case ShaderBuiltin::ViewportArrayIndex: return "SV_VIEWPORTARRAYINDEX";
        case ShaderBuiltin::VertexIndex: return "SV_VERTEXID";
        case ShaderBuiltin::PrimitiveIndex: return "SV_PRIMITIVEID";
        case ShaderBuiltin::InstanceIndex: return "SV_INSTANCEID";
        case ShaderBuiltin::FrontFacing: return "SV_ISFRONTFACE";
        case ShaderBuiltin::SampleIndex: return "SV_SAMPLEINDEX";
        case ShaderBuiltin::FragDepth: return "SV_DEPTH";
        case ShaderBuiltin::Coverage: return "SV_COVERAGE";
        case ShaderBuiltin::StencilRef: return "SV_STENCILREF";
        default: return {};
    }
}

bool BuildHlslField(
    const HlslShaderDesc& reflection,
    std::string_view name,
    HlslShaderTypeId typeId,
    uint32_t offset,
    uint32_t size,
    ShaderInterfaceFieldDesc& result,
    vector<ShaderDiagnostic>& diagnostics,
    const ShaderInterfaceNormalizationOptions& options,
    ShaderStage stage,
    uint32_t group,
    uint32_t binding,
    size_t depth) {
    if (static_cast<size_t>(typeId) >= reflection.Types.size() || depth > reflection.Types.size()) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidReflection,
            fmt::format("field '{}' refers to invalid HLSL type {}", name, typeId.Value),
            group,
            binding);
        return false;
    }
    const HlslShaderTypeDesc& type = reflection.Types[static_cast<size_t>(typeId)];
    result.Name = string{name};
    result.Offset = offset;
    result.Size = size;
    result.Type.Scalar = MapHlslScalar(type.Type);
    const bool isStruct = !type.Members.empty();
    result.Type.Rows = isStruct ? 1u : std::max(type.Rows, 1u);
    result.Type.Columns = isStruct ? 1u : std::max(type.Columns, 1u);
    result.Type.ArrayCount = std::max(type.Elements, 1u);
    result.Type.ByteSize = size;
    result.Type.RowMajor = type.Class == HlslShaderVariableClass::MATRIX_ROWS;
    if (type.Elements > 0 && size % type.Elements == 0) {
        result.Type.ArrayStride = size / type.Elements;
    }
    if (result.Type.Rows > 1 && result.Type.Columns > 1) {
        const uint32_t vectorCount = result.Type.RowMajor ? result.Type.Rows : result.Type.Columns;
        uint32_t matrixSize = result.Type.ArrayStride != 0 ? result.Type.ArrayStride : size;
        if (vectorCount != 0 && matrixSize % vectorCount == 0) {
            result.Type.MatrixStride = matrixSize / vectorCount;
        }
    }

    const uint32_t elementSize = result.Type.ArrayStride != 0 ? result.Type.ArrayStride : size;
    for (size_t i = 0; i < type.Members.size(); ++i) {
        const HlslShaderTypeMember& member = type.Members[i];
        if (static_cast<size_t>(member.Type) >= reflection.Types.size()) {
            AddDiagnostic(
                diagnostics,
                options,
                stage,
                ShaderDiagnosticCode::InvalidReflection,
                fmt::format("field '{}.{}' refers to invalid HLSL type {}", name, member.Name, member.Type.Value),
                group,
                binding);
            return false;
        }
        const uint32_t relativeOffset = member.Offset;
        const uint32_t nextOffset = i + 1 < type.Members.size()
                                        ? type.Members[i + 1].Offset
                                        : elementSize;
        if (relativeOffset >= elementSize || nextOffset <= relativeOffset || nextOffset > elementSize) {
            AddDiagnostic(
                diagnostics,
                options,
                stage,
                ShaderDiagnosticCode::InvalidReflection,
                fmt::format("field '{}.{}' has invalid range [{}, {}) in {} bytes", name, member.Name, relativeOffset, nextOffset, elementSize),
                group,
                binding);
            return false;
        }
        ShaderInterfaceFieldDesc child;
        if (!BuildHlslField(
                reflection,
                member.Name,
                member.Type,
                offset + relativeOffset,
                nextOffset - relativeOffset,
                child,
                diagnostics,
                options,
                stage,
                group,
                binding,
                depth + 1)) {
            return false;
        }
        result.Members.emplace_back(std::move(child));
    }
    if (result.Members.empty() && result.Type.Scalar == ShaderScalarType::Unknown) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::UnsupportedType,
            fmt::format("field '{}' uses unsupported HLSL type {}", name, static_cast<uint32_t>(type.Type)),
            group,
            binding);
        return false;
    }
    return true;
}

bool BuildSpirvField(
    const SpirvShaderDesc& reflection,
    std::string_view name,
    uint32_t typeIndex,
    uint32_t offset,
    uint32_t size,
    ShaderInterfaceFieldDesc& result,
    vector<ShaderDiagnostic>& diagnostics,
    const ShaderInterfaceNormalizationOptions& options,
    ShaderStage stage,
    uint32_t group,
    uint32_t binding,
    size_t depth) {
    if (typeIndex >= reflection.Types.size() || depth > reflection.Types.size()) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidReflection,
            fmt::format("field '{}' refers to invalid SPIR-V type {}", name, typeIndex),
            group,
            binding);
        return false;
    }
    const SpirvTypeInfo& type = reflection.Types[typeIndex];
    result.Name = string{name};
    result.Offset = offset;
    result.Size = size;
    result.Type.Scalar = MapSpirvScalar(type.BaseType);
    result.Type.Rows = type.Columns > 1 ? std::max(type.VectorSize, 1u) : 1u;
    result.Type.Columns = type.Columns > 1 ? type.Columns : std::max(type.VectorSize, 1u);
    result.Type.ArrayCount = std::max(type.ArraySize, 1u);
    result.Type.ArrayStride = type.ArrayStride;
    result.Type.MatrixStride = type.MatrixStride;
    result.Type.ByteSize = size;
    result.Type.RowMajor = type.RowMajor;
    for (const SpirvTypeMember& member : type.Members) {
        if (member.Size == 0 || member.Offset >= size || member.Size > size - member.Offset) {
            AddDiagnostic(
                diagnostics,
                options,
                stage,
                ShaderDiagnosticCode::InvalidReflection,
                fmt::format("field '{}.{}' has invalid SPIR-V range", name, member.Name),
                group,
                binding);
            return false;
        }
        ShaderInterfaceFieldDesc child;
        if (!BuildSpirvField(
                reflection,
                member.Name,
                member.TypeIndex,
                offset + member.Offset,
                member.Size,
                child,
                diagnostics,
                options,
                stage,
                group,
                binding,
                depth + 1)) {
            return false;
        }
        if (member.ArrayStride != 0) child.Type.ArrayStride = member.ArrayStride;
        if (member.ArraySize != 0) child.Type.ArrayCount = member.ArraySize;
        if (member.MatrixStride != 0) child.Type.MatrixStride = member.MatrixStride;
        if (child.Type.Rows > 1 && child.Type.Columns > 1) {
            // DXC transposes HLSL matrix types when lowering to SPIR-V, so the
            // SPIR-V major decoration is the inverse of the source ABI view.
            child.Type.RowMajor = !member.RowMajor;
        }
        result.Members.emplace_back(std::move(child));
    }
    if (result.Members.empty() && result.Type.Scalar == ShaderScalarType::Unknown) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::UnsupportedType,
            fmt::format("field '{}' uses unsupported SPIR-V base type {}", name, static_cast<uint32_t>(type.BaseType)),
            group,
            binding);
        return false;
    }
    return true;
}

void SortFields(vector<ShaderInterfaceFieldDesc>& fields) {
    for (ShaderInterfaceFieldDesc& field : fields) SortFields(field.Members);
    std::ranges::sort(fields, [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.Offset, lhs.Name) < std::tie(rhs.Offset, rhs.Name);
    });
}

void CanonicalizeBindingGroups(vector<ShaderBindingGroupInterfaceDesc>& groups) {
    for (ShaderBindingGroupInterfaceDesc& group : groups) {
        for (ShaderBindingDesc& binding : group.Bindings) {
            if (binding.Buffer.has_value()) SortFields(binding.Buffer->Fields);
        }
        std::ranges::sort(group.Bindings, {}, &ShaderBindingDesc::BindingIndex);
    }
    std::ranges::sort(groups, {}, &ShaderBindingGroupInterfaceDesc::GroupIndex);
}

void CanonicalizeIo(vector<ShaderStageIoDesc>& values) {
    std::ranges::sort(values, [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.Builtin, lhs.Location, lhs.SemanticName, lhs.SemanticIndex) <
               std::tie(rhs.Builtin, rhs.Location, rhs.SemanticName, rhs.SemanticIndex);
    });
    uint32_t logicalLocation = 0;
    for (ShaderStageIoDesc& value : values) {
        if (value.Builtin != ShaderBuiltin::None) continue;
        if (value.SemanticName == "SV_TARGET") {
            value.Location = value.SemanticIndex;
        } else {
            value.Location = logicalLocation++;
        }
    }
    std::ranges::sort(values, [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.Builtin, lhs.Location, lhs.SemanticName, lhs.SemanticIndex) <
               std::tie(rhs.Builtin, rhs.Location, rhs.SemanticName, rhs.SemanticIndex);
    });
}

void CanonicalizePushConstants(vector<ShaderPushConstantRangeDesc>& ranges) {
    for (ShaderPushConstantRangeDesc& range : ranges) SortFields(range.Fields);
    std::ranges::sort(ranges, [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.Offset, lhs.Size, lhs.Name) < std::tie(rhs.Offset, rhs.Size, rhs.Name);
    });
}

void Canonicalize(ShaderStageInterfaceDesc& value) {
    CanonicalizeBindingGroups(value.BindingGroups);
    CanonicalizePushConstants(value.PushConstants);
    CanonicalizeIo(value.Inputs);
    CanonicalizeIo(value.Outputs);
}

void Canonicalize(ShaderInterfaceDesc& value) {
    CanonicalizeBindingGroups(value.BindingGroups);
    CanonicalizePushConstants(value.PushConstants);
    CanonicalizeIo(value.VertexInputs);
    CanonicalizeIo(value.VertexOutputs);
    CanonicalizeIo(value.PixelInputs);
    CanonicalizeIo(value.PixelOutputs);
}

ShaderBindingGroupInterfaceDesc& FindOrAddGroup(
    vector<ShaderBindingGroupInterfaceDesc>& groups,
    uint32_t groupIndex) {
    const auto it = std::ranges::find(groups, groupIndex, &ShaderBindingGroupInterfaceDesc::GroupIndex);
    if (it != groups.end()) return *it;
    return groups.emplace_back(ShaderBindingGroupInterfaceDesc{.GroupIndex = groupIndex});
}

bool IsPushConstantBinding(
    const ShaderInterfaceNormalizationOptions& options,
    uint32_t group,
    uint32_t binding) noexcept {
    return std::ranges::find(options.PushConstantBindings, ShaderBindingLocation{group, binding}) !=
           options.PushConstantBindings.end();
}

Nullable<const HlslShaderBufferDesc*> FindHlslCBuffer(
    const HlslShaderDesc& reflection,
    std::string_view name) noexcept {
    return reflection.FindCBufferByName(name);
}

bool BuildHlslBuffer(
    const HlslShaderDesc& reflection,
    const HlslShaderBufferDesc& source,
    uint32_t group,
    uint32_t binding,
    ShaderStage stage,
    const ShaderInterfaceNormalizationOptions& options,
    vector<ShaderDiagnostic>& diagnostics,
    ShaderBufferInterfaceDesc& result) {
    if (source.Size == 0) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidReflection,
            fmt::format("constant buffer '{}' has zero size", source.Name),
            group,
            binding);
        return false;
    }
    result.ByteSize = source.Size;
    vector<ShaderInterfaceFieldDesc> fields;
    for (size_t variableIndex : source.Variables) {
        if (variableIndex >= reflection.Variables.size()) {
            AddDiagnostic(
                diagnostics,
                options,
                stage,
                ShaderDiagnosticCode::InvalidReflection,
                fmt::format("constant buffer '{}' refers to invalid variable {}", source.Name, variableIndex),
                group,
                binding);
            return false;
        }
        const HlslShaderVariableDesc& variable = reflection.Variables[variableIndex];
        if (variable.Size == 0 || variable.StartOffset >= source.Size || variable.Size > source.Size - variable.StartOffset) {
            AddDiagnostic(
                diagnostics,
                options,
                stage,
                ShaderDiagnosticCode::InvalidReflection,
                fmt::format("constant buffer field '{}.{}' has an invalid range", source.Name, variable.Name),
                group,
                binding);
            return false;
        }
        ShaderInterfaceFieldDesc field;
        if (!BuildHlslField(
                reflection,
                variable.Name,
                variable.Type,
                variable.StartOffset,
                variable.Size,
                field,
                diagnostics,
                options,
                stage,
                group,
                binding,
                0)) {
            return false;
        }
        fields.emplace_back(std::move(field));
    }
    if (source.IsViewInHlsl && fields.size() == 1 && !fields.front().Members.empty()) {
        result.Fields = std::move(fields.front().Members);
    } else {
        result.Fields = std::move(fields);
    }
    SortFields(result.Fields);
    return true;
}

bool BuildSpirvBuffer(
    const SpirvShaderDesc& reflection,
    uint32_t typeIndex,
    uint32_t byteSize,
    bool unwrapView,
    uint32_t group,
    uint32_t binding,
    ShaderStage stage,
    const ShaderInterfaceNormalizationOptions& options,
    vector<ShaderDiagnostic>& diagnostics,
    ShaderBufferInterfaceDesc& result) {
    if (typeIndex >= reflection.Types.size() || byteSize == 0) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidReflection,
            fmt::format("buffer at {}:{} has invalid SPIR-V type or size", group, binding),
            group,
            binding);
        return false;
    }
    result.ByteSize = byteSize;
    const SpirvTypeInfo& rootType = reflection.Types[typeIndex];
    ShaderInterfaceFieldDesc root;
    if (!BuildSpirvField(
            reflection,
            rootType.Name,
            typeIndex,
            0,
            byteSize,
            root,
            diagnostics,
            options,
            stage,
            group,
            binding,
            0)) {
        return false;
    }
    if (!root.Members.empty()) {
        result.Fields = std::move(root.Members);
    } else {
        result.Fields.emplace_back(std::move(root));
    }
    if (unwrapView && result.Fields.size() == 1 && !result.Fields.front().Members.empty()) {
        result.Fields = std::move(result.Fields.front().Members);
    }
    result.ElementStride = rootType.ArrayStride;
    SortFields(result.Fields);
    return true;
}

bool AddHlslBinding(
    const HlslShaderDesc& reflection,
    const HlslInputBindDesc& resource,
    ShaderStage stage,
    const ShaderInterfaceNormalizationOptions& options,
    vector<ShaderDiagnostic>& diagnostics,
    ShaderStageInterfaceDesc& output) {
    const bool hasVkSet = resource.VkSet.has_value();
    const bool hasVkBinding = resource.VkBinding.has_value();
    if (hasVkSet != hasVkBinding ||
        (hasVkSet && (*resource.VkSet != resource.Space || *resource.VkBinding != resource.BindPoint))) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidBinding,
            fmt::format("resource '{}' has inconsistent D3D and Vulkan binding metadata", resource.Name),
            resource.Space,
            resource.BindPoint);
        return false;
    }
    const uint32_t groupIndex = resource.Space;
    const uint32_t bindingIndex = resource.BindPoint;
    if (resource.Name.empty()) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidBinding,
            "shader resource has an empty name",
            groupIndex,
            bindingIndex);
        return false;
    }

    if (resource.Type == HlslShaderInputType::CBUFFER &&
        IsPushConstantBinding(options, groupIndex, bindingIndex)) {
        const auto cbuffer = FindHlslCBuffer(reflection, resource.Name);
        if (!cbuffer.HasValue()) {
            AddDiagnostic(
                diagnostics,
                options,
                stage,
                ShaderDiagnosticCode::InvalidReflection,
                fmt::format("push constant '{}' has no matching HLSL buffer", resource.Name),
                groupIndex,
                bindingIndex);
            return false;
        }
        ShaderBufferInterfaceDesc buffer;
        if (!BuildHlslBuffer(
                reflection,
                *cbuffer.Get(),
                groupIndex,
                bindingIndex,
                stage,
                options,
                diagnostics,
                buffer)) {
            return false;
        }
        output.PushConstants.emplace_back(ShaderPushConstantRangeDesc{
            .Name = resource.Name,
            .Offset = 0,
            .Size = buffer.ByteSize,
            .Stages = stage,
            .Fields = std::move(buffer.Fields)});
        return true;
    }

    ShaderBindingDesc binding{
        .Name = resource.Name,
        .BindingIndex = bindingIndex,
        .Kind = ShaderBindingKind::Unknown,
        .Access = ShaderResourceAccess::ReadOnly,
        .Count = resource.BindCount,
        .Stages = stage};
    switch (resource.Type) {
        case HlslShaderInputType::CBUFFER: {
            binding.Kind = ShaderBindingKind::ConstantBuffer;
            const auto cbuffer = FindHlslCBuffer(reflection, resource.Name);
            if (!cbuffer.HasValue()) {
                AddDiagnostic(
                    diagnostics,
                    options,
                    stage,
                    ShaderDiagnosticCode::InvalidReflection,
                    fmt::format("constant buffer resource '{}' has no matching buffer", resource.Name),
                    groupIndex,
                    bindingIndex);
                return false;
            }
            binding.Buffer.emplace();
            if (!BuildHlslBuffer(
                    reflection,
                    *cbuffer.Get(),
                    groupIndex,
                    bindingIndex,
                    stage,
                    options,
                    diagnostics,
                    *binding.Buffer)) {
                return false;
            }
            break;
        }
        case HlslShaderInputType::TEXTURE:
            binding.Kind = IsBufferDimension(resource.Dimension)
                               ? ShaderBindingKind::TypedBuffer
                               : ShaderBindingKind::SampledTexture;
            binding.Texture = ShaderTextureInterfaceDesc{
                .Dimension = MapHlslDimension(resource.Dimension),
                .SampleType = MapHlslSampleType(resource.ReturnType),
                .Arrayed = IsHlslArrayDimension(resource.Dimension),
                .Multisampled = IsHlslMultisampled(resource.Dimension),
                .Depth = false};
            break;
        case HlslShaderInputType::SAMPLER:
            binding.Kind = ShaderBindingKind::Sampler;
            break;
        case HlslShaderInputType::STRUCTURED:
            binding.Kind = ShaderBindingKind::StructuredBuffer;
            binding.Buffer.emplace();
            binding.Buffer->ElementStride = resource.NumSamples;
            break;
        case HlslShaderInputType::BYTEADDRESS:
            binding.Kind = ShaderBindingKind::RawBuffer;
            binding.Buffer.emplace();
            break;
        case HlslShaderInputType::UAV_RWTYPED:
            binding.Kind = IsBufferDimension(resource.Dimension)
                               ? ShaderBindingKind::TypedBuffer
                               : ShaderBindingKind::StorageTexture;
            binding.Access = ShaderResourceAccess::ReadWrite;
            binding.Texture = ShaderTextureInterfaceDesc{
                .Dimension = MapHlslDimension(resource.Dimension),
                .SampleType = MapHlslSampleType(resource.ReturnType),
                .Arrayed = IsHlslArrayDimension(resource.Dimension),
                .Multisampled = IsHlslMultisampled(resource.Dimension),
                .Depth = false};
            break;
        case HlslShaderInputType::UAV_RWSTRUCTURED:
        case HlslShaderInputType::UAV_APPEND_STRUCTURED:
        case HlslShaderInputType::UAV_CONSUME_STRUCTURED:
        case HlslShaderInputType::UAV_RWSTRUCTURED_WITH_COUNTER:
            binding.Kind = ShaderBindingKind::StructuredBuffer;
            binding.Access = resource.Type == HlslShaderInputType::UAV_CONSUME_STRUCTURED
                                 ? ShaderResourceAccess::ReadOnly
                                 : ShaderResourceAccess::ReadWrite;
            binding.Buffer.emplace();
            binding.Buffer->ElementStride = resource.NumSamples;
            break;
        case HlslShaderInputType::UAV_RWBYTEADDRESS:
            binding.Kind = ShaderBindingKind::RawBuffer;
            binding.Access = ShaderResourceAccess::ReadWrite;
            binding.Buffer.emplace();
            break;
        case HlslShaderInputType::RTACCELERATIONSTRUCTURE:
            binding.Kind = ShaderBindingKind::AccelerationStructure;
            break;
        default:
            AddDiagnostic(
                diagnostics,
                options,
                stage,
                ShaderDiagnosticCode::UnsupportedType,
                fmt::format("resource '{}' has unsupported HLSL binding type {}", resource.Name, static_cast<uint32_t>(resource.Type)),
                groupIndex,
                bindingIndex);
            return false;
    }
    ShaderBindingGroupInterfaceDesc& group = FindOrAddGroup(output.BindingGroups, groupIndex);
    if (std::ranges::find(group.Bindings, bindingIndex, &ShaderBindingDesc::BindingIndex) != group.Bindings.end()) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::DuplicateBinding,
            fmt::format("group {} contains duplicate binding {}", groupIndex, bindingIndex),
            groupIndex,
            bindingIndex);
        return false;
    }
    group.Bindings.emplace_back(std::move(binding));
    return true;
}

ShaderResourceAccess MapSpirvAccess(const SpirvResourceBinding& resource) noexcept {
    if (resource.ReadOnly && !resource.WriteOnly) return ShaderResourceAccess::ReadOnly;
    if (!resource.ReadOnly && resource.WriteOnly) return ShaderResourceAccess::WriteOnly;
    return ShaderResourceAccess::ReadWrite;
}

bool IsRawSpirvBufferType(const SpirvShaderDesc& reflection, uint32_t typeIndex, size_t depth = 0) {
    if (typeIndex >= reflection.Types.size() || depth > reflection.Types.size()) return false;
    const SpirvTypeInfo& type = reflection.Types[typeIndex];
    if (type.BaseType == SpirvBaseType::UInt32 && type.Members.empty()) return true;
    if (type.Members.size() != 1) return false;
    const SpirvTypeMember& member = type.Members.front();
    return member.ArrayStride == sizeof(uint32_t) &&
           IsRawSpirvBufferType(reflection, member.TypeIndex, depth + 1);
}

ShaderBindingKind MapSpirvBufferKind(
    const SpirvShaderDesc& reflection,
    const SpirvResourceBinding& resource) {
    const string type = UpperAscii(resource.HlslType);
    if (type.find("BYTEADDRESSBUFFER") != string::npos ||
        IsRawSpirvBufferType(reflection, resource.TypeIndex)) {
        return ShaderBindingKind::RawBuffer;
    }
    return ShaderBindingKind::StructuredBuffer;
}

bool AddSpirvBinding(
    const SpirvShaderDesc& reflection,
    const SpirvResourceBinding& resource,
    ShaderStage stage,
    const ShaderInterfaceNormalizationOptions& options,
    vector<ShaderDiagnostic>& diagnostics,
    ShaderStageInterfaceDesc& output) {
    if ((resource.HlslRegister.has_value() != resource.HlslSpace.has_value()) ||
        (resource.HlslRegister.has_value() &&
         (*resource.HlslRegister != resource.Binding || *resource.HlslSpace != resource.Set))) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidBinding,
            fmt::format("resource '{}' has inconsistent SPIR-V and HLSL binding metadata", resource.Name),
            resource.Set,
            resource.Binding);
        return false;
    }
    if (resource.Name.empty()) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidBinding,
            "SPIR-V resource has an empty name",
            resource.Set,
            resource.Binding);
        return false;
    }
    ShaderBindingDesc binding{
        .Name = resource.Name,
        .BindingIndex = resource.Binding,
        .Kind = ShaderBindingKind::Unknown,
        .Access = MapSpirvAccess(resource),
        .Count = resource.IsUnboundedArray ? 0u : std::max(resource.ArraySize, 1u),
        .Stages = stage};
    switch (resource.Kind) {
        case SpirvResourceKind::UniformBuffer:
            binding.Kind = ShaderBindingKind::ConstantBuffer;
            binding.Access = ShaderResourceAccess::ReadOnly;
            binding.Buffer.emplace();
            if (!BuildSpirvBuffer(
                    reflection,
                    resource.TypeIndex,
                    resource.UniformBufferSize,
                    resource.IsViewInHlsl,
                    resource.Set,
                    resource.Binding,
                    stage,
                    options,
                    diagnostics,
                    *binding.Buffer)) {
                return false;
            }
            break;
        case SpirvResourceKind::StorageBuffer:
            binding.Kind = MapSpirvBufferKind(reflection, resource);
            binding.Buffer.emplace();
            if (resource.TypeIndex < reflection.Types.size()) {
                const SpirvTypeInfo& type = reflection.Types[resource.TypeIndex];
                binding.Buffer->ByteSize = type.Size;
                binding.Buffer->ElementStride = type.ArrayStride;
                if (binding.Kind == ShaderBindingKind::StructuredBuffer &&
                    binding.Buffer->ElementStride == 0 && type.Members.size() == 1) {
                    binding.Buffer->ElementStride = type.Members.front().ArrayStride;
                }
            }
            break;
        case SpirvResourceKind::SampledImage:
        case SpirvResourceKind::SeparateImage:
            binding.Kind = resource.ImageInfo.has_value() && resource.ImageInfo->Dim == SpirvImageDim::Buffer
                               ? ShaderBindingKind::TypedBuffer
                               : ShaderBindingKind::SampledTexture;
            break;
        case SpirvResourceKind::StorageImage:
            binding.Kind = resource.ImageInfo.has_value() && resource.ImageInfo->Dim == SpirvImageDim::Buffer
                               ? ShaderBindingKind::TypedBuffer
                               : ShaderBindingKind::StorageTexture;
            break;
        case SpirvResourceKind::SeparateSampler:
            binding.Kind = ShaderBindingKind::Sampler;
            binding.Access = ShaderResourceAccess::ReadOnly;
            break;
        case SpirvResourceKind::AccelerationStructure:
            binding.Kind = ShaderBindingKind::AccelerationStructure;
            binding.Access = ShaderResourceAccess::ReadOnly;
            break;
        default:
            AddDiagnostic(
                diagnostics,
                options,
                stage,
                ShaderDiagnosticCode::UnsupportedType,
                fmt::format("resource '{}' has unsupported SPIR-V kind {}", resource.Name, static_cast<uint32_t>(resource.Kind)),
                resource.Set,
                resource.Binding);
            return false;
    }
    if (resource.ImageInfo.has_value()) {
        const SpirvImageInfo& image = *resource.ImageInfo;
        binding.Texture = ShaderTextureInterfaceDesc{
            .Dimension = MapSpirvDimension(image.Dim),
            .SampleType = MapSpirvSampleType(reflection, image),
            .Arrayed = image.Arrayed,
            .Multisampled = image.Multisampled,
            .Depth = image.Depth};
    }
    ShaderBindingGroupInterfaceDesc& group = FindOrAddGroup(output.BindingGroups, resource.Set);
    if (std::ranges::find(group.Bindings, resource.Binding, &ShaderBindingDesc::BindingIndex) != group.Bindings.end()) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::DuplicateBinding,
            fmt::format("group {} contains duplicate binding {}", resource.Set, resource.Binding),
            resource.Set,
            resource.Binding);
        return false;
    }
    group.Bindings.emplace_back(std::move(binding));
    return true;
}

ShaderStageIoDesc NormalizeHlslIo(const HlslSignatureParameterDesc& source) {
    ParsedSemantic semantic = ParseSemantic(source.SemanticName, source.SemanticIndex);
    const uint32_t components = source.Mask == 0 ? 1u : std::popcount(static_cast<uint32_t>(source.Mask));
    const ShaderBuiltin builtin = MapHlslBuiltin(source.SystemValueType, semantic.Name);
    const ShaderScalarType scalar = MapHlslIoScalar(source.ComponentType);
    if (builtin != ShaderBuiltin::None) {
        semantic.Name = string{BuiltinSemantic(builtin)};
        semantic.Index = 0;
    }
    return ShaderStageIoDesc{
        .SemanticName = std::move(semantic.Name),
        .SemanticIndex = semantic.Index,
        .Location = builtin == ShaderBuiltin::None ? source.Register : 0,
        .Builtin = builtin,
        .Type = ShaderValueTypeDesc{
            .Scalar = scalar,
            .Rows = 1,
            .Columns = components,
            .ArrayCount = 1,
            .ByteSize = ScalarByteSize(scalar) * components}};
}

std::optional<ShaderStageIoDesc> NormalizeSpirvIo(
    const SpirvShaderDesc& reflection,
    const SpirvStageIo& source,
    ShaderStage stage,
    const ShaderInterfaceNormalizationOptions& options,
    vector<ShaderDiagnostic>& diagnostics) {
    if (source.TypeIndex >= reflection.Types.size()) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidReflection,
            fmt::format("stage IO '{}' refers to invalid SPIR-V type {}", source.Name, source.TypeIndex));
        return std::nullopt;
    }
    const SpirvTypeInfo& type = reflection.Types[source.TypeIndex];
    ParsedSemantic semantic = ParseSemantic(
        source.HlslSemantic.empty() ? std::string_view{source.Name} : std::string_view{source.HlslSemantic},
        0);
    ShaderBuiltin builtin = MapSemanticBuiltin(semantic.Name);
    if (builtin == ShaderBuiltin::None && source.BuiltIn.has_value()) {
        builtin = MapSpirvBuiltin(*source.BuiltIn);
    }
    if (builtin != ShaderBuiltin::None) {
        semantic.Name = string{BuiltinSemantic(builtin)};
        semantic.Index = 0;
    }
    ShaderValueTypeDesc valueType{
        .Scalar = MapSpirvScalar(type.BaseType),
        .Rows = type.Columns > 1 ? std::max(type.VectorSize, 1u) : 1u,
        .Columns = type.Columns > 1 ? type.Columns : std::max(type.VectorSize, 1u),
        .ArrayCount = std::max(type.ArraySize, 1u),
        .ArrayStride = type.ArrayStride,
        .MatrixStride = type.MatrixStride,
        .ByteSize = type.Size,
        .RowMajor = type.RowMajor};
    if (valueType.Scalar == ShaderScalarType::Unknown) {
        AddDiagnostic(
            diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::UnsupportedType,
            fmt::format("stage IO '{}' uses unsupported SPIR-V type", source.Name));
        return std::nullopt;
    }
    return ShaderStageIoDesc{
        .SemanticName = std::move(semantic.Name),
        .SemanticIndex = semantic.Index,
        .Location = builtin == ShaderBuiltin::None ? source.Location : 0,
        .Builtin = builtin,
        .Type = valueType};
}

bool SameBindingPayload(const ShaderBindingDesc& lhs, const ShaderBindingDesc& rhs) {
    ShaderBindingDesc left = lhs;
    ShaderBindingDesc right = rhs;
    left.Stages = ShaderStage::UNKNOWN;
    right.Stages = ShaderStage::UNKNOWN;
    return left == right;
}

bool MergeBindingGroups(
    vector<ShaderBindingGroupInterfaceDesc>& output,
    const vector<ShaderBindingGroupInterfaceDesc>& incoming,
    const ShaderDiagnosticContext& context,
    ShaderStage stage,
    vector<ShaderDiagnostic>& diagnostics) {
    for (const ShaderBindingGroupInterfaceDesc& sourceGroup : incoming) {
        ShaderBindingGroupInterfaceDesc& destination = FindOrAddGroup(output, sourceGroup.GroupIndex);
        for (const ShaderBindingDesc& sourceBinding : sourceGroup.Bindings) {
            const auto current = std::ranges::find(
                destination.Bindings,
                sourceBinding.BindingIndex,
                &ShaderBindingDesc::BindingIndex);
            if (current == destination.Bindings.end()) {
                destination.Bindings.emplace_back(sourceBinding);
                continue;
            }
            if (!SameBindingPayload(*current, sourceBinding)) {
                AddMergeDiagnostic(
                    diagnostics,
                    context,
                    stage,
                    ShaderDiagnosticCode::IncompatibleBinding,
                    fmt::format(
                        "group {} binding {} has incompatible declarations across stages",
                        sourceGroup.GroupIndex,
                        sourceBinding.BindingIndex),
                    sourceGroup.GroupIndex,
                    sourceBinding.BindingIndex);
                return false;
            }
            current->Stages |= sourceBinding.Stages;
        }
    }
    return true;
}

bool RangesOverlap(uint32_t lhsOffset, uint32_t lhsSize, uint32_t rhsOffset, uint32_t rhsSize) noexcept {
    const uint64_t lhsEnd = static_cast<uint64_t>(lhsOffset) + lhsSize;
    const uint64_t rhsEnd = static_cast<uint64_t>(rhsOffset) + rhsSize;
    return lhsOffset < rhsEnd && rhsOffset < lhsEnd;
}

bool MergePushConstants(
    vector<ShaderPushConstantRangeDesc>& output,
    const vector<ShaderPushConstantRangeDesc>& incoming,
    const ShaderDiagnosticContext& context,
    ShaderStage stage,
    vector<ShaderDiagnostic>& diagnostics) {
    for (const ShaderPushConstantRangeDesc& source : incoming) {
        bool merged = false;
        for (ShaderPushConstantRangeDesc& current : output) {
            if (current.Offset == source.Offset && current.Size == source.Size &&
                current.Name == source.Name && current.Fields == source.Fields) {
                current.Stages |= source.Stages;
                merged = true;
                break;
            }
            if (RangesOverlap(current.Offset, current.Size, source.Offset, source.Size)) {
                AddMergeDiagnostic(
                    diagnostics,
                    context,
                    stage,
                    ShaderDiagnosticCode::IncompatibleBinding,
                    fmt::format("push constant '{}' overlaps incompatible range '{}'", source.Name, current.Name));
                return false;
            }
        }
        if (!merged) output.emplace_back(source);
    }
    return true;
}

bool RequiresVertexOutput(ShaderBuiltin builtin) noexcept {
    switch (builtin) {
        case ShaderBuiltin::FrontFacing:
        case ShaderBuiltin::SampleIndex:
        case ShaderBuiltin::PrimitiveIndex:
        case ShaderBuiltin::Coverage: return false;
        default: return true;
    }
}

bool IsIoLinkCompatible(const ShaderStageIoDesc& output, const ShaderStageIoDesc& input) noexcept {
    if (output.Builtin != input.Builtin) return false;
    if (input.Builtin == ShaderBuiltin::None) {
        if (output.Location != input.Location) return false;
        if (!output.SemanticName.empty() && !input.SemanticName.empty() &&
            (output.SemanticName != input.SemanticName || output.SemanticIndex != input.SemanticIndex)) {
            return false;
        }
    }
    return output.Type == input.Type;
}

bool ValidateLeafType(const ShaderValueTypeDesc& type) noexcept {
    if (type.Scalar == ShaderScalarType::Unknown ||
        !IsEnumInRange(type.Scalar, ShaderScalarType::Float64) ||
        type.Rows == 0 || type.Rows > 4 || type.Columns == 0 || type.Columns > 4 ||
        type.ArrayCount == 0 || type.ByteSize == 0) {
        return false;
    }
    const uint32_t scalarSize = ScalarByteSize(type.Scalar);
    if (scalarSize == 0) return false;
    const bool matrix = type.Rows > 1 && type.Columns > 1;
    if (type.RowMajor && !matrix) return false;
    uint64_t requiredElementBytes = 0;
    if (matrix) {
        if (type.MatrixStride == 0) return false;
        const uint32_t vectorCount = type.RowMajor ? type.Rows : type.Columns;
        const uint32_t vectorWidth = type.RowMajor ? type.Columns : type.Rows;
        requiredElementBytes =
            static_cast<uint64_t>(vectorCount - 1) * type.MatrixStride +
            static_cast<uint64_t>(vectorWidth) * scalarSize;
    } else {
        requiredElementBytes = static_cast<uint64_t>(type.Rows) * type.Columns * scalarSize;
    }
    if (type.ArrayCount == 1) return requiredElementBytes <= type.ByteSize;
    if (type.ArrayStride == 0 || requiredElementBytes > type.ArrayStride) return false;
    return static_cast<uint64_t>(type.ArrayCount - 1) * type.ArrayStride +
               requiredElementBytes <=
           type.ByteSize;
}

bool ValidateFields(
    const vector<ShaderInterfaceFieldDesc>& fields,
    uint32_t byteSize,
    uint32_t depth = 0) noexcept {
    if (depth > 64) return false;
    for (size_t i = 0; i < fields.size(); ++i) {
        const ShaderInterfaceFieldDesc& field = fields[i];
        if (field.Name.empty() || field.Size == 0 || field.Offset >= byteSize || field.Size > byteSize - field.Offset ||
            field.Type.Rows == 0 || field.Type.Columns == 0 || field.Type.ArrayCount == 0 ||
            field.Type.ByteSize != field.Size ||
            !IsEnumInRange(field.Type.Scalar, ShaderScalarType::Float64)) {
            return false;
        }
        if ((field.Type.ArrayCount > 1 &&
             (field.Type.ArrayStride == 0 ||
              static_cast<uint64_t>(field.Type.ArrayCount - 1) * field.Type.ArrayStride >= field.Size)) ||
            (field.Type.Rows > 1 && field.Type.Columns > 1 && field.Type.MatrixStride == 0)) {
            return false;
        }
        if (field.Members.empty()) {
            if (!ValidateLeafType(field.Type)) return false;
        } else {
            if (field.Type.Scalar != ShaderScalarType::Unknown || field.Type.MatrixStride != 0 ||
                field.Type.RowMajor) {
                return false;
            }
            const uint32_t firstElementSize = field.Type.ArrayCount > 1
                                                  ? field.Type.ArrayStride
                                                  : field.Size;
            const uint64_t firstElementEnd =
                static_cast<uint64_t>(field.Offset) + firstElementSize;
            for (const ShaderInterfaceFieldDesc& member : field.Members) {
                if (member.Offset < field.Offset || member.Offset >= firstElementEnd ||
                    member.Size > firstElementEnd - member.Offset) {
                    return false;
                }
            }
        }
        for (size_t j = 0; j < i; ++j) {
            const ShaderInterfaceFieldDesc& previous = fields[j];
            if (previous.Name == field.Name ||
                RangesOverlap(previous.Offset, previous.Size, field.Offset, field.Size)) {
                return false;
            }
        }
        if (!field.Members.empty() && !ValidateFields(field.Members, byteSize, depth + 1)) return false;
    }
    return true;
}

bool ValidateTexture(const ShaderBindingDesc& binding) noexcept {
    if (!binding.Texture.has_value() ||
        binding.Texture->Dimension == ShaderTextureDimension::Unknown ||
        !IsEnumInRange(binding.Texture->Dimension, ShaderTextureDimension::Buffer) ||
        binding.Texture->SampleType == ShaderSampleType::Unknown ||
        !IsEnumInRange(binding.Texture->SampleType, ShaderSampleType::Depth)) {
        return false;
    }
    if (binding.Kind == ShaderBindingKind::TypedBuffer) {
        return binding.Texture->Dimension == ShaderTextureDimension::Buffer &&
               !binding.Texture->Arrayed && !binding.Texture->Multisampled &&
               !binding.Texture->Depth;
    }
    if (binding.Texture->Dimension == ShaderTextureDimension::Buffer ||
        (binding.Texture->Dimension == ShaderTextureDimension::Dim3D && binding.Texture->Arrayed) ||
        (binding.Texture->Multisampled &&
         binding.Texture->Dimension != ShaderTextureDimension::Dim2D)) {
        return false;
    }
    return binding.Kind != ShaderBindingKind::StorageTexture ||
           !binding.Texture->Multisampled;
}

bool ValidateBindingGroups(const vector<ShaderBindingGroupInterfaceDesc>& groups) noexcept {
    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].Bindings.empty()) return false;
        for (size_t j = 0; j < i; ++j) {
            if (groups[j].GroupIndex == groups[i].GroupIndex) return false;
        }
        for (size_t j = 0; j < groups[i].Bindings.size(); ++j) {
            const ShaderBindingDesc& binding = groups[i].Bindings[j];
            if (binding.Name.empty() || binding.Kind == ShaderBindingKind::Unknown ||
                !IsEnumInRange(binding.Kind, ShaderBindingKind::AccelerationStructure) ||
                !IsEnumInRange(binding.Access, ShaderResourceAccess::ReadWrite) ||
                !AreShaderStagesValid(binding.Stages)) {
                return false;
            }
            for (size_t k = 0; k < j; ++k) {
                if (groups[i].Bindings[k].BindingIndex == binding.BindingIndex ||
                    groups[i].Bindings[k].Name == binding.Name) {
                    return false;
                }
            }
            const bool bufferKind = binding.Kind == ShaderBindingKind::ConstantBuffer ||
                                    binding.Kind == ShaderBindingKind::StructuredBuffer ||
                                    binding.Kind == ShaderBindingKind::RawBuffer;
            if (bufferKind != binding.Buffer.has_value()) return false;
            if (binding.Kind == ShaderBindingKind::ConstantBuffer) {
                if (!binding.Buffer.has_value() || binding.Buffer->ByteSize == 0 ||
                    binding.Count != 1 || binding.Access != ShaderResourceAccess::ReadOnly ||
                    !ValidateFields(binding.Buffer->Fields, binding.Buffer->ByteSize)) {
                    return false;
                }
            } else if (binding.Kind == ShaderBindingKind::StructuredBuffer &&
                       binding.Buffer->ElementStride == 0) {
                return false;
            } else if (binding.Kind == ShaderBindingKind::RawBuffer &&
                       binding.Buffer->ElementStride != 0) {
                return false;
            }
            const bool textureKind = binding.Kind == ShaderBindingKind::SampledTexture ||
                                     binding.Kind == ShaderBindingKind::StorageTexture ||
                                     binding.Kind == ShaderBindingKind::TypedBuffer;
            if (textureKind != binding.Texture.has_value()) return false;
            if (textureKind && !ValidateTexture(binding)) return false;
            if ((binding.Kind == ShaderBindingKind::SampledTexture ||
                 binding.Kind == ShaderBindingKind::Sampler ||
                 binding.Kind == ShaderBindingKind::AccelerationStructure) &&
                binding.Access != ShaderResourceAccess::ReadOnly) {
                return false;
            }
        }
    }
    return true;
}

bool ValidateIo(const vector<ShaderStageIoDesc>& values) noexcept {
    for (size_t i = 0; i < values.size(); ++i) {
        const ShaderStageIoDesc& value = values[i];
        if (value.Type.Scalar == ShaderScalarType::Unknown ||
            !IsEnumInRange(value.Type.Scalar, ShaderScalarType::Float64) ||
            !IsEnumInRange(value.Builtin, ShaderBuiltin::StencilRef) ||
            !ValidateLeafType(value.Type) ||
            (value.Builtin == ShaderBuiltin::None && value.SemanticName.empty())) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            const ShaderStageIoDesc& previous = values[j];
            if (value.Builtin != ShaderBuiltin::None && previous.Builtin == value.Builtin) return false;
            if (value.Builtin == ShaderBuiltin::None && previous.Builtin == ShaderBuiltin::None &&
                previous.Location == value.Location) {
                return false;
            }
        }
    }
    return true;
}

bool ValidatePushConstants(const vector<ShaderPushConstantRangeDesc>& ranges) noexcept {
    for (size_t i = 0; i < ranges.size(); ++i) {
        const ShaderPushConstantRangeDesc& range = ranges[i];
        if (range.Name.empty() || range.Size == 0 || !AreShaderStagesValid(range.Stages) ||
            range.Offset > std::numeric_limits<uint32_t>::max() - range.Size ||
            !ValidateFields(range.Fields, range.Offset + range.Size)) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (RangesOverlap(range.Offset, range.Size, ranges[j].Offset, ranges[j].Size) &&
                !(range.Offset == ranges[j].Offset && range.Size == ranges[j].Size &&
                  range.Name == ranges[j].Name && range.Fields == ranges[j].Fields)) {
                return false;
            }
        }
    }
    return true;
}

bool ValidateInterfaceStages(
    const vector<ShaderBindingGroupInterfaceDesc>& groups,
    const vector<ShaderPushConstantRangeDesc>& pushConstants,
    ShaderStages allowed) noexcept {
    for (const ShaderBindingGroupInterfaceDesc& group : groups) {
        for (const ShaderBindingDesc& binding : group.Bindings) {
            if ((binding.Stages.value() & ~allowed.value()) != 0) return false;
        }
    }
    return std::ranges::all_of(pushConstants, [&](const ShaderPushConstantRangeDesc& range) {
        return (range.Stages.value() & ~allowed.value()) == 0;
    });
}

class InterfaceWriter {
public:
    void U8(uint8_t value) { Data.emplace_back(static_cast<byte>(value)); }
    void U32(uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i) U8(static_cast<uint8_t>((value >> (i * 8)) & 0xffu));
    }
    void String(std::string_view value) {
        U32(static_cast<uint32_t>(value.size()));
        for (char c : value) U8(static_cast<uint8_t>(c));
    }

    vector<byte> Data;
};

void WriteValueType(InterfaceWriter& writer, const ShaderValueTypeDesc& value) {
    writer.U8(static_cast<uint8_t>(value.Scalar));
    writer.U32(value.Rows);
    writer.U32(value.Columns);
    writer.U32(value.ArrayCount);
    writer.U32(value.ArrayStride);
    writer.U32(value.MatrixStride);
    writer.U32(value.ByteSize);
    writer.U8(value.RowMajor ? 1 : 0);
}

void WriteFields(InterfaceWriter& writer, const vector<ShaderInterfaceFieldDesc>& fields) {
    writer.U32(static_cast<uint32_t>(fields.size()));
    for (const ShaderInterfaceFieldDesc& field : fields) {
        writer.String(field.Name);
        writer.U32(field.Offset);
        writer.U32(field.Size);
        WriteValueType(writer, field.Type);
        WriteFields(writer, field.Members);
    }
}

void WriteGroups(InterfaceWriter& writer, const vector<ShaderBindingGroupInterfaceDesc>& groups) {
    writer.U32(static_cast<uint32_t>(groups.size()));
    for (const ShaderBindingGroupInterfaceDesc& group : groups) {
        writer.U32(group.GroupIndex);
        writer.U32(static_cast<uint32_t>(group.Bindings.size()));
        for (const ShaderBindingDesc& binding : group.Bindings) {
            writer.String(binding.Name);
            writer.U32(binding.BindingIndex);
            writer.U8(static_cast<uint8_t>(binding.Kind));
            writer.U8(static_cast<uint8_t>(binding.Access));
            writer.U32(binding.Count);
            writer.U32(binding.Stages.value());
            writer.U8(binding.Buffer.has_value() ? 1 : 0);
            if (binding.Buffer.has_value()) {
                writer.U32(binding.Buffer->ByteSize);
                writer.U32(binding.Buffer->ElementStride);
                WriteFields(writer, binding.Buffer->Fields);
            }
            writer.U8(binding.Texture.has_value() ? 1 : 0);
            if (binding.Texture.has_value()) {
                writer.U8(static_cast<uint8_t>(binding.Texture->Dimension));
                writer.U8(static_cast<uint8_t>(binding.Texture->SampleType));
                writer.U8(binding.Texture->Arrayed ? 1 : 0);
                writer.U8(binding.Texture->Multisampled ? 1 : 0);
                writer.U8(binding.Texture->Depth ? 1 : 0);
            }
        }
    }
}

void WriteIo(InterfaceWriter& writer, const vector<ShaderStageIoDesc>& values) {
    writer.U32(static_cast<uint32_t>(values.size()));
    for (const ShaderStageIoDesc& value : values) {
        writer.String(value.SemanticName);
        writer.U32(value.SemanticIndex);
        writer.U32(value.Location);
        writer.U8(static_cast<uint8_t>(value.Builtin));
        WriteValueType(writer, value.Type);
    }
}

void WritePushConstants(InterfaceWriter& writer, const vector<ShaderPushConstantRangeDesc>& ranges) {
    writer.U32(static_cast<uint32_t>(ranges.size()));
    for (const ShaderPushConstantRangeDesc& range : ranges) {
        writer.String(range.Name);
        writer.U32(range.Offset);
        writer.U32(range.Size);
        writer.U32(range.Stages.value());
        WriteFields(writer, range.Fields);
    }
}

void WriteCompute(InterfaceWriter& writer, const std::optional<ShaderComputeInterfaceDesc>& value) {
    writer.U8(value.has_value() ? 1 : 0);
    if (value.has_value()) {
        writer.U32(value->GroupSizeX);
        writer.U32(value->GroupSizeY);
        writer.U32(value->GroupSizeZ);
    }
}

class InterfaceReader {
public:
    explicit InterfaceReader(std::span<const byte> data) noexcept : _data(data) {}

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

    bool Bool(bool& value) noexcept {
        uint8_t raw = 0;
        if (!U8(raw) || raw > 1) return false;
        value = raw != 0;
        return true;
    }

    bool String(string& value) {
        uint32_t size = 0;
        if (!U32(size) || size > kMaxStringBytes || size > _data.size() - _offset) return false;
        value.assign(reinterpret_cast<const char*>(_data.data() + _offset), size);
        _offset += size;
        return true;
    }

    bool AtEnd() const noexcept { return _offset == _data.size(); }

    static constexpr uint32_t kMaxElementCount = 1u << 20;
    static constexpr uint32_t kMaxStringBytes = 16u << 20;

private:
    std::span<const byte> _data;
    size_t _offset{0};
};

bool ReadValueType(InterfaceReader& reader, ShaderValueTypeDesc& value) noexcept {
    uint8_t scalar = 0;
    return reader.U8(scalar) && scalar <= static_cast<uint8_t>(ShaderScalarType::Float64) &&
           (value.Scalar = static_cast<ShaderScalarType>(scalar), true) &&
           reader.U32(value.Rows) && reader.U32(value.Columns) &&
           reader.U32(value.ArrayCount) && reader.U32(value.ArrayStride) &&
           reader.U32(value.MatrixStride) && reader.U32(value.ByteSize) &&
           reader.Bool(value.RowMajor);
}

bool ReadFields(
    InterfaceReader& reader,
    vector<ShaderInterfaceFieldDesc>& fields,
    uint32_t depth = 0) {
    uint32_t count = 0;
    if (depth > 64 || !reader.U32(count) || count > InterfaceReader::kMaxElementCount) return false;
    fields.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ShaderInterfaceFieldDesc field;
        if (!reader.String(field.Name) || !reader.U32(field.Offset) || !reader.U32(field.Size) ||
            !ReadValueType(reader, field.Type) || !ReadFields(reader, field.Members, depth + 1)) {
            return false;
        }
        fields.emplace_back(std::move(field));
    }
    return true;
}

bool ReadGroups(InterfaceReader& reader, vector<ShaderBindingGroupInterfaceDesc>& groups) {
    uint32_t groupCount = 0;
    if (!reader.U32(groupCount) || groupCount > InterfaceReader::kMaxElementCount) return false;
    groups.reserve(groupCount);
    constexpr uint32_t supportedStages = static_cast<uint32_t>(ShaderStage::Vertex) |
                                         static_cast<uint32_t>(ShaderStage::Pixel) |
                                         static_cast<uint32_t>(ShaderStage::Compute);
    for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
        ShaderBindingGroupInterfaceDesc group;
        uint32_t bindingCount = 0;
        if (!reader.U32(group.GroupIndex) || !reader.U32(bindingCount) ||
            bindingCount > InterfaceReader::kMaxElementCount) {
            return false;
        }
        group.Bindings.reserve(bindingCount);
        for (uint32_t bindingIndex = 0; bindingIndex < bindingCount; ++bindingIndex) {
            ShaderBindingDesc binding;
            uint8_t kind = 0;
            uint8_t access = 0;
            uint32_t stages = 0;
            bool hasBuffer = false;
            if (!reader.String(binding.Name) || !reader.U32(binding.BindingIndex) ||
                !reader.U8(kind) || kind > static_cast<uint8_t>(ShaderBindingKind::AccelerationStructure) ||
                !reader.U8(access) || access > static_cast<uint8_t>(ShaderResourceAccess::ReadWrite) ||
                !reader.U32(binding.Count) || !reader.U32(stages) || (stages & ~supportedStages) != 0 ||
                !reader.Bool(hasBuffer)) {
                return false;
            }
            binding.Kind = static_cast<ShaderBindingKind>(kind);
            binding.Access = static_cast<ShaderResourceAccess>(access);
            binding.Stages = ShaderStages{stages};
            if (hasBuffer) {
                ShaderBufferInterfaceDesc buffer;
                if (!reader.U32(buffer.ByteSize) || !reader.U32(buffer.ElementStride) ||
                    !ReadFields(reader, buffer.Fields)) {
                    return false;
                }
                binding.Buffer = std::move(buffer);
            }
            bool hasTexture = false;
            if (!reader.Bool(hasTexture)) return false;
            if (hasTexture) {
                ShaderTextureInterfaceDesc texture;
                uint8_t dimension = 0;
                uint8_t sampleType = 0;
                if (!reader.U8(dimension) || dimension > static_cast<uint8_t>(ShaderTextureDimension::Buffer) ||
                    !reader.U8(sampleType) || sampleType > static_cast<uint8_t>(ShaderSampleType::Depth) ||
                    !reader.Bool(texture.Arrayed) || !reader.Bool(texture.Multisampled) ||
                    !reader.Bool(texture.Depth)) {
                    return false;
                }
                texture.Dimension = static_cast<ShaderTextureDimension>(dimension);
                texture.SampleType = static_cast<ShaderSampleType>(sampleType);
                binding.Texture = texture;
            }
            group.Bindings.emplace_back(std::move(binding));
        }
        groups.emplace_back(std::move(group));
    }
    return true;
}

bool ReadIo(InterfaceReader& reader, vector<ShaderStageIoDesc>& values) {
    uint32_t count = 0;
    if (!reader.U32(count) || count > InterfaceReader::kMaxElementCount) return false;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ShaderStageIoDesc value;
        uint8_t builtin = 0;
        if (!reader.String(value.SemanticName) || !reader.U32(value.SemanticIndex) ||
            !reader.U32(value.Location) || !reader.U8(builtin) ||
            builtin > static_cast<uint8_t>(ShaderBuiltin::StencilRef) ||
            !ReadValueType(reader, value.Type)) {
            return false;
        }
        value.Builtin = static_cast<ShaderBuiltin>(builtin);
        values.emplace_back(std::move(value));
    }
    return true;
}

bool ReadPushConstants(InterfaceReader& reader, vector<ShaderPushConstantRangeDesc>& ranges) {
    uint32_t count = 0;
    if (!reader.U32(count) || count > InterfaceReader::kMaxElementCount) return false;
    ranges.reserve(count);
    constexpr uint32_t supportedStages = static_cast<uint32_t>(ShaderStage::Vertex) |
                                         static_cast<uint32_t>(ShaderStage::Pixel) |
                                         static_cast<uint32_t>(ShaderStage::Compute);
    for (uint32_t i = 0; i < count; ++i) {
        ShaderPushConstantRangeDesc range;
        uint32_t stages = 0;
        if (!reader.String(range.Name) || !reader.U32(range.Offset) || !reader.U32(range.Size) ||
            !reader.U32(stages) || (stages & ~supportedStages) != 0 ||
            !ReadFields(reader, range.Fields)) {
            return false;
        }
        range.Stages = ShaderStages{stages};
        ranges.emplace_back(std::move(range));
    }
    return true;
}

bool ReadCompute(InterfaceReader& reader, std::optional<ShaderComputeInterfaceDesc>& value) noexcept {
    bool present = false;
    if (!reader.Bool(present)) return false;
    if (!present) {
        value.reset();
        return true;
    }
    ShaderComputeInterfaceDesc compute;
    if (!reader.U32(compute.GroupSizeX) || !reader.U32(compute.GroupSizeY) ||
        !reader.U32(compute.GroupSizeZ)) {
        return false;
    }
    value = compute;
    return true;
}

}  // namespace

ShaderStageInterfaceBuildResult NormalizeHlslInterface(
    const HlslShaderDesc& reflection,
    ShaderStage stage,
    const ShaderInterfaceNormalizationOptions& options) noexcept {
    ShaderStageInterfaceBuildResult result;
    try {
        if (!IsSupportedStage(stage)) {
            AddDiagnostic(
                result.Diagnostics,
                options,
                stage,
                ShaderDiagnosticCode::UnsupportedStage,
                fmt::format("unsupported HLSL shader stage {}", stage));
            return result;
        }
        ShaderStageInterfaceDesc interface{.Stage = stage};
        for (const HlslInputBindDesc& resource : reflection.BoundResources) {
            if (!AddHlslBinding(reflection, resource, stage, options, result.Diagnostics, interface)) return result;
        }
        for (const HlslSignatureParameterDesc& input : reflection.InputParameters) {
            if (input.ReadWriteMask == 0) continue;
            ShaderStageIoDesc value = NormalizeHlslIo(input);
            if (value.Type.Scalar == ShaderScalarType::Unknown) {
                AddDiagnostic(
                    result.Diagnostics,
                    options,
                    stage,
                    ShaderDiagnosticCode::UnsupportedType,
                    fmt::format("input semantic '{}' has unsupported component type", input.SemanticName));
                return result;
            }
            interface.Inputs.emplace_back(std::move(value));
        }
        for (const HlslSignatureParameterDesc& output : reflection.OutputParameters) {
            ShaderStageIoDesc value = NormalizeHlslIo(output);
            if (value.Type.Scalar == ShaderScalarType::Unknown) {
                AddDiagnostic(
                    result.Diagnostics,
                    options,
                    stage,
                    ShaderDiagnosticCode::UnsupportedType,
                    fmt::format("output semantic '{}' has unsupported component type", output.SemanticName));
                return result;
            }
            interface.Outputs.emplace_back(std::move(value));
        }
        if (stage == ShaderStage::Compute) {
            if (reflection.GroupSizeX == 0 || reflection.GroupSizeY == 0 || reflection.GroupSizeZ == 0) {
                AddDiagnostic(
                    result.Diagnostics,
                    options,
                    stage,
                    ShaderDiagnosticCode::InvalidComputeGroupSize,
                    "compute shader has a zero thread-group dimension");
                return result;
            }
            interface.Compute = ShaderComputeInterfaceDesc{
                .GroupSizeX = reflection.GroupSizeX,
                .GroupSizeY = reflection.GroupSizeY,
                .GroupSizeZ = reflection.GroupSizeZ};
        }
        Canonicalize(interface);
        if (!IsShaderStageInterfaceValid(interface)) {
            AddDiagnostic(
                result.Diagnostics,
                options,
                stage,
                ShaderDiagnosticCode::InvalidReflection,
                "normalized HLSL interface failed structural validation");
            return result;
        }
        result.Interface = std::move(interface);
    } catch (const std::exception& error) {
        AddDiagnostic(
            result.Diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidReflection,
            fmt::format("failed to normalize HLSL reflection: {}", error.what()));
    } catch (...) {
        AddDiagnostic(
            result.Diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidReflection,
            "failed to normalize HLSL reflection");
    }
    return result;
}

ShaderStageInterfaceBuildResult NormalizeSpirvInterface(
    const SpirvShaderDesc& reflection,
    ShaderStage stage,
    const ShaderInterfaceNormalizationOptions& options) noexcept {
    ShaderStageInterfaceBuildResult result;
    try {
        if (!IsSupportedStage(stage)) {
            AddDiagnostic(
                result.Diagnostics,
                options,
                stage,
                ShaderDiagnosticCode::UnsupportedStage,
                fmt::format("unsupported SPIR-V shader stage {}", stage));
            return result;
        }
        ShaderStageInterfaceDesc interface{.Stage = stage};
        for (const SpirvResourceBinding& resource : reflection.ResourceBindings) {
            if (!AddSpirvBinding(reflection, resource, stage, options, result.Diagnostics, interface)) return result;
        }
        for (const SpirvPushConstantRange& range : reflection.ConstantRanges) {
            ShaderBufferInterfaceDesc buffer;
            if (!BuildSpirvBuffer(
                    reflection,
                    range.TypeIndex,
                    range.Size,
                    range.IsViewInHlsl,
                    0,
                    0,
                    stage,
                    options,
                    result.Diagnostics,
                    buffer)) {
                return result;
            }
            interface.PushConstants.emplace_back(ShaderPushConstantRangeDesc{
                .Name = range.Name,
                .Offset = range.Offset,
                .Size = range.Size,
                .Stages = stage,
                .Fields = std::move(buffer.Fields)});
        }
        if (stage != ShaderStage::Compute) {
            for (const SpirvStageIo& input : reflection.StageInputs) {
                auto value = NormalizeSpirvIo(reflection, input, stage, options, result.Diagnostics);
                if (!value.has_value()) return result;
                interface.Inputs.emplace_back(std::move(*value));
            }
            for (const SpirvStageIo& output : reflection.StageOutputs) {
                auto value = NormalizeSpirvIo(reflection, output, stage, options, result.Diagnostics);
                if (!value.has_value()) return result;
                interface.Outputs.emplace_back(std::move(*value));
            }
        }
        if (stage == ShaderStage::Compute) {
            if (!reflection.ComputeInfo.has_value() ||
                reflection.ComputeInfo->LocalSizeX == 0 ||
                reflection.ComputeInfo->LocalSizeY == 0 ||
                reflection.ComputeInfo->LocalSizeZ == 0) {
                AddDiagnostic(
                    result.Diagnostics,
                    options,
                    stage,
                    ShaderDiagnosticCode::InvalidComputeGroupSize,
                    "compute shader has no valid SPIR-V workgroup size");
                return result;
            }
            interface.Compute = ShaderComputeInterfaceDesc{
                .GroupSizeX = reflection.ComputeInfo->LocalSizeX,
                .GroupSizeY = reflection.ComputeInfo->LocalSizeY,
                .GroupSizeZ = reflection.ComputeInfo->LocalSizeZ};
        }
        Canonicalize(interface);
        if (!IsShaderStageInterfaceValid(interface)) {
            AddDiagnostic(
                result.Diagnostics,
                options,
                stage,
                ShaderDiagnosticCode::InvalidReflection,
                "normalized SPIR-V interface failed structural validation");
            return result;
        }
        result.Interface = std::move(interface);
    } catch (const std::exception& error) {
        AddDiagnostic(
            result.Diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidReflection,
            fmt::format("failed to normalize SPIR-V reflection: {}", error.what()));
    } catch (...) {
        AddDiagnostic(
            result.Diagnostics,
            options,
            stage,
            ShaderDiagnosticCode::InvalidReflection,
            "failed to normalize SPIR-V reflection");
    }
    return result;
}

ShaderInterfaceBuildResult MergeGraphicsStageInterfaces(
    const ShaderStageInterfaceDesc& vertex,
    const ShaderDiagnosticContext& context) noexcept {
    ShaderInterfaceBuildResult result;
    if (vertex.Stage != ShaderStage::Vertex || !IsShaderStageInterfaceValid(vertex)) {
        AddMergeDiagnostic(
            result.Diagnostics,
            context,
            vertex.Stage,
            ShaderDiagnosticCode::UnsupportedStage,
            "graphics interface requires a valid vertex stage");
        return result;
    }
    ShaderInterfaceDesc interface{
        .Kind = ShaderProgramKind::Graphics,
        .BindingGroups = vertex.BindingGroups,
        .PushConstants = vertex.PushConstants,
        .VertexInputs = vertex.Inputs,
        .VertexOutputs = vertex.Outputs};
    Canonicalize(interface);
    if (!IsShaderInterfaceValid(interface)) {
        AddMergeDiagnostic(
            result.Diagnostics,
            context,
            ShaderStage::Vertex,
            ShaderDiagnosticCode::InvalidReflection,
            "merged vertex-only graphics interface is invalid");
        return result;
    }
    result.Interface = std::move(interface);
    return result;
}

ShaderInterfaceBuildResult MergeGraphicsStageInterfaces(
    const ShaderStageInterfaceDesc& vertex,
    const ShaderStageInterfaceDesc& pixel,
    const ShaderDiagnosticContext& context) noexcept {
    ShaderInterfaceBuildResult result;
    if (vertex.Stage != ShaderStage::Vertex || pixel.Stage != ShaderStage::Pixel ||
        !IsShaderStageInterfaceValid(vertex) || !IsShaderStageInterfaceValid(pixel)) {
        AddMergeDiagnostic(
            result.Diagnostics,
            context,
            ShaderStage::Graphics,
            ShaderDiagnosticCode::UnsupportedStage,
            "graphics interface requires valid vertex and pixel stages");
        return result;
    }
    ShaderInterfaceDesc interface{
        .Kind = ShaderProgramKind::Graphics,
        .BindingGroups = vertex.BindingGroups,
        .PushConstants = vertex.PushConstants,
        .VertexInputs = vertex.Inputs,
        .VertexOutputs = vertex.Outputs,
        .PixelInputs = pixel.Inputs,
        .PixelOutputs = pixel.Outputs};
    if (!MergeBindingGroups(
            interface.BindingGroups,
            pixel.BindingGroups,
            context,
            ShaderStage::Pixel,
            result.Diagnostics) ||
        !MergePushConstants(
            interface.PushConstants,
            pixel.PushConstants,
            context,
            ShaderStage::Pixel,
            result.Diagnostics)) {
        return result;
    }
    for (const ShaderStageIoDesc& input : pixel.Inputs) {
        if (!RequiresVertexOutput(input.Builtin)) continue;
        const auto output = std::ranges::find_if(vertex.Outputs, [&](const ShaderStageIoDesc& candidate) {
            return IsIoLinkCompatible(candidate, input);
        });
        if (output == vertex.Outputs.end()) {
            AddMergeDiagnostic(
                result.Diagnostics,
                context,
                ShaderStage::Pixel,
                ShaderDiagnosticCode::IncompatibleStageIo,
                fmt::format(
                    "pixel input '{}{}' at location {} has no compatible vertex output",
                    input.SemanticName,
                    input.SemanticIndex,
                    input.Location));
            return result;
        }
    }
    Canonicalize(interface);
    if (!IsShaderInterfaceValid(interface)) {
        AddMergeDiagnostic(
            result.Diagnostics,
            context,
            ShaderStage::Graphics,
            ShaderDiagnosticCode::InvalidReflection,
            "merged graphics interface is invalid");
        return result;
    }
    result.Interface = std::move(interface);
    return result;
}

ShaderInterfaceBuildResult BuildComputeShaderInterface(
    const ShaderStageInterfaceDesc& compute,
    const ShaderDiagnosticContext& context) noexcept {
    ShaderInterfaceBuildResult result;
    if (compute.Stage != ShaderStage::Compute || !compute.Compute.has_value() ||
        !IsShaderStageInterfaceValid(compute)) {
        AddMergeDiagnostic(
            result.Diagnostics,
            context,
            compute.Stage,
            ShaderDiagnosticCode::UnsupportedStage,
            "compute interface requires a valid compute stage");
        return result;
    }
    ShaderInterfaceDesc interface{
        .Kind = ShaderProgramKind::Compute,
        .BindingGroups = compute.BindingGroups,
        .PushConstants = compute.PushConstants,
        .Compute = compute.Compute};
    Canonicalize(interface);
    if (!IsShaderInterfaceValid(interface)) {
        AddMergeDiagnostic(
            result.Diagnostics,
            context,
            ShaderStage::Compute,
            ShaderDiagnosticCode::InvalidReflection,
            "compute interface is invalid");
        return result;
    }
    result.Interface = std::move(interface);
    return result;
}

bool IsShaderStageInterfaceValid(const ShaderStageInterfaceDesc& interface) noexcept {
    try {
        if (!IsSupportedStage(interface.Stage) || !ValidateBindingGroups(interface.BindingGroups) ||
            !ValidatePushConstants(interface.PushConstants) || !ValidateIo(interface.Inputs) ||
            !ValidateIo(interface.Outputs) ||
            !ValidateInterfaceStages(
                interface.BindingGroups,
                interface.PushConstants,
                ShaderStages{interface.Stage})) {
            return false;
        }
        if (interface.Stage == ShaderStage::Compute) {
            return interface.Compute.has_value() && interface.Compute->GroupSizeX != 0 &&
                   interface.Compute->GroupSizeY != 0 && interface.Compute->GroupSizeZ != 0 &&
                   interface.Inputs.empty() && interface.Outputs.empty();
        }
        return !interface.Compute.has_value();
    } catch (...) {
        return false;
    }
}

bool IsShaderInterfaceValid(const ShaderInterfaceDesc& interface) noexcept {
    try {
        if (!IsEnumInRange(interface.Kind, ShaderProgramKind::Compute) ||
            !ValidateBindingGroups(interface.BindingGroups) ||
            !ValidatePushConstants(interface.PushConstants) ||
            !ValidateIo(interface.VertexInputs) || !ValidateIo(interface.VertexOutputs) ||
            !ValidateIo(interface.PixelInputs) || !ValidateIo(interface.PixelOutputs)) {
            return false;
        }
        if (interface.Kind == ShaderProgramKind::Compute) {
            return interface.Compute.has_value() && interface.Compute->GroupSizeX != 0 &&
                   interface.Compute->GroupSizeY != 0 && interface.Compute->GroupSizeZ != 0 &&
                   interface.VertexInputs.empty() && interface.VertexOutputs.empty() &&
                   interface.PixelInputs.empty() && interface.PixelOutputs.empty() &&
                   ValidateInterfaceStages(
                       interface.BindingGroups,
                       interface.PushConstants,
                       ShaderStage::Compute);
        }
        return !interface.Compute.has_value() &&
               ValidateInterfaceStages(
                   interface.BindingGroups,
                   interface.PushConstants,
                   ShaderStage::Vertex | ShaderStage::Pixel);
    } catch (...) {
        return false;
    }
}

std::optional<vector<byte>> SerializeShaderStageInterface(
    const ShaderStageInterfaceDesc& interface) noexcept {
    try {
        if (!IsShaderStageInterfaceValid(interface)) return std::nullopt;
        ShaderStageInterfaceDesc normalized = interface;
        Canonicalize(normalized);
        InterfaceWriter writer;
        writer.U32(static_cast<uint32_t>(normalized.Stage));
        WriteGroups(writer, normalized.BindingGroups);
        WritePushConstants(writer, normalized.PushConstants);
        WriteIo(writer, normalized.Inputs);
        WriteIo(writer, normalized.Outputs);
        WriteCompute(writer, normalized.Compute);
        return std::move(writer.Data);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<vector<byte>> SerializeShaderInterface(
    const ShaderInterfaceDesc& interface) noexcept {
    try {
        if (!IsShaderInterfaceValid(interface)) return std::nullopt;
        ShaderInterfaceDesc normalized = interface;
        Canonicalize(normalized);
        InterfaceWriter writer;
        writer.U8(static_cast<uint8_t>(normalized.Kind));
        WriteGroups(writer, normalized.BindingGroups);
        WritePushConstants(writer, normalized.PushConstants);
        WriteIo(writer, normalized.VertexInputs);
        WriteIo(writer, normalized.VertexOutputs);
        WriteIo(writer, normalized.PixelInputs);
        WriteIo(writer, normalized.PixelOutputs);
        WriteCompute(writer, normalized.Compute);
        return std::move(writer.Data);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<ShaderStageInterfaceDesc> DeserializeShaderStageInterface(
    std::span<const byte> data) noexcept {
    try {
        InterfaceReader reader{data};
        ShaderStageInterfaceDesc result;
        uint32_t stage = 0;
        if (!reader.U32(stage) ||
            (stage != static_cast<uint32_t>(ShaderStage::Vertex) &&
             stage != static_cast<uint32_t>(ShaderStage::Pixel) &&
             stage != static_cast<uint32_t>(ShaderStage::Compute)) ||
            !ReadGroups(reader, result.BindingGroups) ||
            !ReadPushConstants(reader, result.PushConstants) ||
            !ReadIo(reader, result.Inputs) || !ReadIo(reader, result.Outputs) ||
            !ReadCompute(reader, result.Compute) || !reader.AtEnd()) {
            return std::nullopt;
        }
        result.Stage = static_cast<ShaderStage>(stage);
        if (!IsShaderStageInterfaceValid(result)) return std::nullopt;
        const auto canonical = SerializeShaderStageInterface(result);
        if (!canonical.has_value() || !std::ranges::equal(*canonical, data)) return std::nullopt;
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<ShaderInterfaceDesc> DeserializeShaderInterface(
    std::span<const byte> data) noexcept {
    try {
        InterfaceReader reader{data};
        ShaderInterfaceDesc result;
        uint8_t kind = 0;
        if (!reader.U8(kind) || kind > static_cast<uint8_t>(ShaderProgramKind::Compute) ||
            !ReadGroups(reader, result.BindingGroups) ||
            !ReadPushConstants(reader, result.PushConstants) ||
            !ReadIo(reader, result.VertexInputs) || !ReadIo(reader, result.VertexOutputs) ||
            !ReadIo(reader, result.PixelInputs) || !ReadIo(reader, result.PixelOutputs) ||
            !ReadCompute(reader, result.Compute) || !reader.AtEnd()) {
            return std::nullopt;
        }
        result.Kind = static_cast<ShaderProgramKind>(kind);
        if (!IsShaderInterfaceValid(result)) return std::nullopt;
        const auto canonical = SerializeShaderInterface(result);
        if (!canonical.has_value() || !std::ranges::equal(*canonical, data)) return std::nullopt;
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

ShaderHash HashShaderStageInterface(const ShaderStageInterfaceDesc& interface) noexcept {
    const auto bytes = SerializeShaderStageInterface(interface);
    return bytes.has_value() ? HashShaderBytes(*bytes) : ShaderHash{};
}

ShaderHash HashShaderInterface(const ShaderInterfaceDesc& interface) noexcept {
    const auto bytes = SerializeShaderInterface(interface);
    return bytes.has_value() ? HashShaderBytes(*bytes) : ShaderHash{};
}

bool AreShaderBindingsAbiCompatible(
    const ShaderBindingDesc& lhs,
    const ShaderBindingDesc& rhs) noexcept {
    const auto fieldsCompatible = [](const auto& self,
                                     const vector<ShaderInterfaceFieldDesc>& left,
                                     const vector<ShaderInterfaceFieldDesc>& right) noexcept -> bool {
        if (left.size() != right.size()) return false;
        for (size_t i = 0; i < left.size(); ++i) {
            if (left[i].Offset != right[i].Offset || left[i].Size != right[i].Size ||
                left[i].Type != right[i].Type || !self(self, left[i].Members, right[i].Members)) {
                return false;
            }
        }
        return true;
    };
    if (lhs.BindingIndex != rhs.BindingIndex || lhs.Kind != rhs.Kind || lhs.Access != rhs.Access ||
        lhs.Count != rhs.Count || lhs.Buffer.has_value() != rhs.Buffer.has_value() ||
        lhs.Texture.has_value() != rhs.Texture.has_value()) {
        return false;
    }
    if (lhs.Buffer.has_value() &&
        (lhs.Buffer->ByteSize != rhs.Buffer->ByteSize ||
         lhs.Buffer->ElementStride != rhs.Buffer->ElementStride ||
         !fieldsCompatible(fieldsCompatible, lhs.Buffer->Fields, rhs.Buffer->Fields))) {
        return false;
    }
    return !lhs.Texture.has_value() || lhs.Texture == rhs.Texture;
}

bool IsShaderBindingAbiProjectionOf(
    const ShaderBindingDesc& projection,
    const ShaderBindingDesc& complete) noexcept {
    try {
        if (AreShaderBindingsAbiCompatible(projection, complete)) return true;
        if (projection.BindingIndex != complete.BindingIndex ||
            projection.Kind != complete.Kind || projection.Access != complete.Access ||
            projection.Count != complete.Count ||
            projection.Buffer.has_value() != complete.Buffer.has_value() ||
            projection.Texture.has_value() != complete.Texture.has_value() ||
            projection.Kind != ShaderBindingKind::ConstantBuffer ||
            !projection.Buffer.has_value() || !complete.Buffer.has_value() ||
            projection.Buffer->ByteSize > complete.Buffer->ByteSize ||
            projection.Buffer->ElementStride != complete.Buffer->ElementStride) {
            return false;
        }

        struct FieldSignature {
            uint32_t Offset{0};
            uint32_t Size{0};
            ShaderValueTypeDesc Type{};

            bool operator==(const FieldSignature&) const = default;
        };
        const auto appendFields = [](const auto& self,
                                     const vector<ShaderInterfaceFieldDesc>& fields,
                                     vector<FieldSignature>& output,
                                     uint64_t offsetDelta) -> bool {
            for (const ShaderInterfaceFieldDesc& field : fields) {
                if (field.Members.empty()) {
                    const uint64_t offset = offsetDelta + field.Offset;
                    if (offset > std::numeric_limits<uint32_t>::max()) return false;
                    output.emplace_back(FieldSignature{
                        .Offset = static_cast<uint32_t>(offset),
                        .Size = field.Size,
                        .Type = field.Type});
                    continue;
                }
                const uint32_t count = field.Type.ArrayCount > 1 ? field.Type.ArrayCount : 1;
                const uint32_t stride = field.Type.ArrayCount > 1 ? field.Type.ArrayStride : 0;
                for (uint32_t index = 0; index < count; ++index) {
                    const uint64_t elementDelta = offsetDelta +
                                                  static_cast<uint64_t>(index) * stride;
                    if (elementDelta > std::numeric_limits<uint32_t>::max() ||
                        !self(self, field.Members, output, elementDelta)) {
                        return false;
                    }
                }
            }
            return true;
        };

        vector<FieldSignature> projectionFields;
        vector<FieldSignature> completeFields;
        if (!appendFields(appendFields, projection.Buffer->Fields, projectionFields, 0) ||
            !appendFields(appendFields, complete.Buffer->Fields, completeFields, 0)) {
            return false;
        }
        return std::ranges::all_of(projectionFields, [&](const FieldSignature& field) {
            return std::ranges::find(completeFields, field) != completeFields.end();
        });
    } catch (...) {
        return false;
    }
}

}  // namespace radray::shader
