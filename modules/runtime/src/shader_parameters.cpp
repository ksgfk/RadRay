#include <radray/runtime/shader_parameters.h>

#include <algorithm>
#include <cstring>
#include <tuple>

#include <fmt/format.h>

namespace radray {
namespace {

uint32_t ScalarSize(render::ShaderScalarType type) noexcept {
    using enum render::ShaderScalarType;
    switch (type) {
        case Bool:
        case Int32:
        case UInt32:
        case Float32: return 4;
        case Int8:
        case UInt8: return 1;
        case Int16:
        case UInt16:
        case Float16: return 2;
        case Int64:
        case UInt64:
        case Float64: return 8;
        default: return 0;
    }
}

std::string_view LeafName(std::string_view value) noexcept {
    const size_t separator = value.rfind('.');
    return separator == std::string_view::npos ? value : value.substr(separator + 1);
}

void AppendFields(
    const vector<render::ShaderInterfaceFieldDesc>& fields,
    std::string_view bindingName,
    std::string_view prefix,
    vector<ShaderParameterFieldDesc>& output,
    uint32_t offsetDelta = 0) {
    for (const render::ShaderInterfaceFieldDesc& field : fields) {
        string path{prefix};
        if (!path.empty()) path.push_back('.');
        path.append(field.Name);
        if (field.Members.empty()) {
            string qualified{bindingName};
            if (!qualified.empty()) qualified.push_back('.');
            qualified.append(path);
            output.emplace_back(ShaderParameterFieldDesc{
                .Name = std::move(path),
                .QualifiedName = std::move(qualified),
                .Offset = field.Offset + offsetDelta,
                .Size = field.Size,
                .Type = field.Type});
        } else if (field.Type.ArrayCount > 1 && field.Type.ArrayStride != 0) {
            for (uint32_t index = 0; index < field.Type.ArrayCount; ++index) {
                const string indexed = fmt::format("{}[{}]", path, index);
                AppendFields(
                    field.Members,
                    bindingName,
                    indexed,
                    output,
                    offsetDelta + index * field.Type.ArrayStride);
            }
        } else {
            AppendFields(field.Members, bindingName, path, output, offsetDelta);
        }
    }
}

render::ShaderHash HashParameterGroups(
    const vector<render::ShaderBindingGroupInterfaceDesc>& groups,
    render::ShaderProgramKind kind) noexcept {
    render::ShaderInterfaceDesc identity{
        .Kind = kind,
        .BindingGroups = groups,
        .PushConstants = {},
        .VertexInputs = {},
        .VertexOutputs = {},
        .PixelInputs = {},
        .PixelOutputs = {}};
    if (kind == render::ShaderProgramKind::Compute) {
        identity.Compute = render::ShaderComputeInterfaceDesc{};
    }
    return render::HashShaderInterface(identity);
}

bool IsBufferKind(render::ShaderBindingKind kind) noexcept {
    return kind == render::ShaderBindingKind::TypedBuffer ||
           kind == render::ShaderBindingKind::StructuredBuffer ||
           kind == render::ShaderBindingKind::RawBuffer;
}

bool IsBufferCompatible(
    const render::ShaderBindingDesc& binding,
    const render::Buffer& buffer,
    render::BufferRange range) noexcept {
    if (!buffer.IsValid()) return false;
    const render::BufferDescriptor desc = buffer.GetDesc();
    if (desc.Size == 0 || range.Offset >= desc.Size) return false;
    const uint64_t size = range.Size == render::BufferRange::All()
                              ? desc.Size - range.Offset
                              : range.Size;
    if (size == 0 || size > desc.Size - range.Offset) return false;
    if (binding.Buffer.has_value() && binding.Buffer->ElementStride != 0 &&
        (range.Offset % binding.Buffer->ElementStride != 0 ||
         size % binding.Buffer->ElementStride != 0)) {
        return false;
    }
    return binding.Access == render::ShaderResourceAccess::ReadOnly
               ? desc.Usage.HasFlag(render::BufferUse::Resource)
               : desc.Usage.HasFlag(render::BufferUse::UnorderedAccess);
}

render::ShaderSampleType GetTextureFormatClass(render::TextureFormat format) noexcept {
    using enum render::TextureFormat;
    switch (format) {
        case R8_SINT:
        case R16_SINT:
        case RG8_SINT:
        case R32_SINT:
        case RG16_SINT:
        case RGBA8_SINT:
        case RG32_SINT:
        case RGBA16_SINT:
        case RGBA32_SINT: return render::ShaderSampleType::SInt;
        case R8_UINT:
        case R16_UINT:
        case RG8_UINT:
        case R32_UINT:
        case RG16_UINT:
        case RGBA8_UINT:
        case RGB10A2_UINT:
        case RG32_UINT:
        case RGBA16_UINT:
        case RGBA32_UINT: return render::ShaderSampleType::UInt;
        case R8_SNORM:
        case R16_SNORM:
        case RG8_SNORM:
        case RG16_SNORM:
        case RGBA8_SNORM:
        case RGBA16_SNORM: return render::ShaderSampleType::SNorm;
        case R8_UNORM:
        case R16_UNORM:
        case RG8_UNORM:
        case RG16_UNORM:
        case RGBA8_UNORM:
        case RGBA8_UNORM_SRGB:
        case BGRA8_UNORM:
        case BGRA8_UNORM_SRGB:
        case RGB10A2_UNORM:
        case RGBA16_UNORM: return render::ShaderSampleType::UNorm;
        case R16_FLOAT:
        case R32_FLOAT:
        case RG16_FLOAT:
        case RG11B10_FLOAT:
        case RG32_FLOAT:
        case RGBA16_FLOAT:
        case RGBA32_FLOAT: return render::ShaderSampleType::Float;
        case D16_UNORM:
        case D32_FLOAT:
        case D24_UNORM_S8_UINT:
        case D32_FLOAT_S8_UINT: return render::ShaderSampleType::Depth;
        default: return render::ShaderSampleType::Unknown;
    }
}

bool IsSampleTypeCompatible(
    render::ShaderSampleType shaderType,
    render::ShaderSampleType formatType) noexcept {
    if (shaderType == render::ShaderSampleType::Float) {
        return formatType == render::ShaderSampleType::Float ||
               formatType == render::ShaderSampleType::UNorm ||
               formatType == render::ShaderSampleType::SNorm;
    }
    return shaderType == formatType;
}

bool IsTextureDimensionCompatible(
    const render::ShaderTextureInterfaceDesc& interface,
    render::TextureDimension dimension) noexcept {
    using enum render::TextureDimension;
    switch (interface.Dimension) {
        case render::ShaderTextureDimension::Dim1D:
            return interface.Arrayed ? dimension == Dim1DArray : dimension == Dim1D;
        case render::ShaderTextureDimension::Dim2D:
            return interface.Arrayed ? dimension == Dim2DArray : dimension == Dim2D;
        case render::ShaderTextureDimension::Dim3D:
            return !interface.Arrayed && dimension == Dim3D;
        case render::ShaderTextureDimension::Cube:
            return interface.Arrayed ? dimension == CubeArray : dimension == Cube;
        default: return false;
    }
}

bool IsTextureCompatible(
    const render::ShaderBindingDesc& binding,
    const ShaderTextureParameterValue& value) noexcept {
    if (!binding.Texture.has_value() || !value.Texture.IsReady() ||
        value.Texture.Get() == nullptr || !value.Texture->IsValid()) {
        return false;
    }
    const render::TextureDescriptor desc = value.Texture->GetTexture()->GetDesc();
    const render::TextureDimension dimension = value.View.IsDefault() ? desc.Dim : value.View.Dim;
    const render::TextureFormat format = value.View.Format == render::TextureFormat::UNKNOWN
                                             ? desc.Format
                                             : value.View.Format;
    if (!IsTextureDimensionCompatible(*binding.Texture, dimension) ||
        binding.Texture->Multisampled != (desc.SampleCount > 1) ||
        !IsSampleTypeCompatible(binding.Texture->SampleType, GetTextureFormatClass(format))) {
        return false;
    }
    return binding.Kind == render::ShaderBindingKind::StorageTexture
               ? desc.Usage.HasFlag(render::TextureUse::UnorderedAccess)
               : desc.Usage.HasFlag(render::TextureUse::Resource);
}

struct UserBindingOrigin {
    ShaderParameterLocation Location{};
    string Name;
    render::ShaderDiagnosticContext Context{};
};

struct UserFieldOrigin {
    ShaderParameterLocation Location{};
    string Field;
    render::ShaderDiagnosticContext Context{};
};

struct UserGroupMergeResult {
    bool Compatible{true};
    std::optional<uint32_t> Binding{};
    std::optional<render::ShaderDiagnosticContext> RelatedContext{};
    string Detail;
};

const UserBindingOrigin* FindOrigin(
    const vector<UserBindingOrigin>& origins,
    uint32_t groupIndex,
    uint32_t bindingIndex) noexcept {
    const auto origin = std::ranges::find_if(origins, [&](const UserBindingOrigin& value) {
        return value.Location == ShaderParameterLocation{groupIndex, bindingIndex};
    });
    return origin == origins.end() ? nullptr : &*origin;
}

const UserFieldOrigin* FindFieldOrigin(
    const vector<UserFieldOrigin>& origins,
    ShaderParameterLocation location,
    std::string_view field) noexcept {
    const auto origin = std::ranges::find_if(origins, [&](const UserFieldOrigin& value) {
        return value.Location == location && value.Field == field;
    });
    return origin == origins.end() ? nullptr : &*origin;
}

bool RangesOverlap(
    uint32_t lhsOffset,
    uint32_t lhsSize,
    uint32_t rhsOffset,
    uint32_t rhsSize) noexcept {
    const uint64_t lhsEnd = static_cast<uint64_t>(lhsOffset) + lhsSize;
    const uint64_t rhsEnd = static_cast<uint64_t>(rhsOffset) + rhsSize;
    return lhsOffset < rhsEnd && rhsOffset < lhsEnd;
}

void AppendFlatConstantFields(
    const vector<render::ShaderInterfaceFieldDesc>& fields,
    std::string_view prefix,
    vector<render::ShaderInterfaceFieldDesc>& output,
    uint32_t offsetDelta = 0) {
    for (const render::ShaderInterfaceFieldDesc& field : fields) {
        string path{prefix};
        if (!path.empty()) path.push_back('.');
        path.append(field.Name);
        if (field.Members.empty()) {
            render::ShaderInterfaceFieldDesc flattened = field;
            flattened.Name = std::move(path);
            flattened.Offset += offsetDelta;
            output.emplace_back(std::move(flattened));
            continue;
        }
        if (field.Type.ArrayCount > 1) {
            for (uint32_t index = 0; index < field.Type.ArrayCount; ++index) {
                AppendFlatConstantFields(
                    field.Members,
                    fmt::format("{}[{}]", path, index),
                    output,
                    offsetDelta + index * field.Type.ArrayStride);
            }
        } else {
            AppendFlatConstantFields(field.Members, path, output, offsetDelta);
        }
    }
}

void NormalizeUserGroup(render::ShaderBindingGroupInterfaceDesc& group) {
    for (render::ShaderBindingDesc& binding : group.Bindings) {
        if (binding.Kind != render::ShaderBindingKind::ConstantBuffer ||
            !binding.Buffer.has_value()) {
            continue;
        }
        vector<render::ShaderInterfaceFieldDesc> flattened;
        AppendFlatConstantFields(binding.Buffer->Fields, {}, flattened);
        std::ranges::sort(flattened, [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs.Offset, lhs.Name) < std::tie(rhs.Offset, rhs.Name);
        });
        binding.Buffer->Fields = std::move(flattened);
    }
}

void AppendOrigins(
    const render::ShaderBindingGroupInterfaceDesc& group,
    const render::ShaderDiagnosticContext& context,
    vector<UserBindingOrigin>& origins,
    vector<UserFieldOrigin>& fieldOrigins) {
    for (const render::ShaderBindingDesc& binding : group.Bindings) {
        const ShaderParameterLocation location{
            .Group = group.GroupIndex,
            .Binding = binding.BindingIndex};
        origins.emplace_back(UserBindingOrigin{
            .Location = location,
            .Name = binding.Name,
            .Context = context});
        if (!binding.Buffer.has_value()) continue;
        for (const render::ShaderInterfaceFieldDesc& field : binding.Buffer->Fields) {
            fieldOrigins.emplace_back(UserFieldOrigin{
                .Location = location,
                .Field = field.Name,
                .Context = context});
        }
    }
}

UserGroupMergeResult MergeConstantBuffer(
    render::ShaderBindingDesc& destination,
    const render::ShaderBindingDesc& source,
    uint32_t groupIndex,
    const render::ShaderDiagnosticContext& context,
    vector<UserFieldOrigin>& fieldOrigins) {
    if (destination.BindingIndex != source.BindingIndex ||
        destination.Kind != render::ShaderBindingKind::ConstantBuffer ||
        source.Kind != render::ShaderBindingKind::ConstantBuffer ||
        destination.Access != source.Access || destination.Count != source.Count ||
        !destination.Buffer.has_value() || !source.Buffer.has_value() ||
        destination.Texture.has_value() || source.Texture.has_value() ||
        destination.Buffer->ElementStride != source.Buffer->ElementStride) {
        return {
            .Compatible = false,
            .Binding = source.BindingIndex,
            .Detail = "constant-buffer resource metadata is incompatible"};
    }

    const ShaderParameterLocation location{
        .Group = groupIndex,
        .Binding = source.BindingIndex};
    for (const render::ShaderInterfaceFieldDesc& incoming : source.Buffer->Fields) {
        const auto sameName = std::ranges::find(
            destination.Buffer->Fields,
            incoming.Name,
            &render::ShaderInterfaceFieldDesc::Name);
        if (sameName != destination.Buffer->Fields.end()) {
            if (sameName->Offset == incoming.Offset && sameName->Size == incoming.Size &&
                sameName->Type == incoming.Type) {
                continue;
            }
            const UserFieldOrigin* origin = FindFieldOrigin(
                fieldOrigins,
                location,
                sameName->Name);
            return {
                .Compatible = false,
                .Binding = source.BindingIndex,
                .RelatedContext = origin != nullptr
                                      ? std::optional<render::ShaderDiagnosticContext>{origin->Context}
                                      : std::nullopt,
                .Detail = fmt::format(
                    "constant field '{}' changed from byte range [{}..{}) to [{}..{}) or changed type",
                    incoming.Name,
                    sameName->Offset,
                    static_cast<uint64_t>(sameName->Offset) + sameName->Size,
                    incoming.Offset,
                    static_cast<uint64_t>(incoming.Offset) + incoming.Size)};
        }

        const auto overlap = std::ranges::find_if(
            destination.Buffer->Fields,
            [&](const render::ShaderInterfaceFieldDesc& existing) {
                return RangesOverlap(
                    existing.Offset,
                    existing.Size,
                    incoming.Offset,
                    incoming.Size);
            });
        if (overlap != destination.Buffer->Fields.end()) {
            const UserFieldOrigin* origin = FindFieldOrigin(
                fieldOrigins,
                location,
                overlap->Name);
            return {
                .Compatible = false,
                .Binding = source.BindingIndex,
                .RelatedContext = origin != nullptr
                                      ? std::optional<render::ShaderDiagnosticContext>{origin->Context}
                                      : std::nullopt,
                .Detail = fmt::format(
                    "constant field '{}' byte range [{}..{}) overlaps field '{}' range [{}..{})",
                    incoming.Name,
                    incoming.Offset,
                    static_cast<uint64_t>(incoming.Offset) + incoming.Size,
                    overlap->Name,
                    overlap->Offset,
                    static_cast<uint64_t>(overlap->Offset) + overlap->Size)};
        }

        destination.Buffer->Fields.emplace_back(incoming);
        fieldOrigins.emplace_back(UserFieldOrigin{
            .Location = location,
            .Field = incoming.Name,
            .Context = context});
    }
    destination.Buffer->ByteSize = std::max(
        destination.Buffer->ByteSize,
        source.Buffer->ByteSize);
    destination.Stages |= source.Stages;
    std::ranges::sort(destination.Buffer->Fields, [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.Offset, lhs.Name) < std::tie(rhs.Offset, rhs.Name);
    });
    return {};
}

UserGroupMergeResult MergeUserGroup(
    render::ShaderBindingGroupInterfaceDesc& destination,
    const render::ShaderBindingGroupInterfaceDesc& source,
    const render::ShaderDiagnosticContext& context,
    vector<UserBindingOrigin>& origins,
    vector<UserFieldOrigin>& fieldOrigins) {
    for (const render::ShaderBindingDesc& incoming : source.Bindings) {
        auto sameLocation = std::ranges::find(
            destination.Bindings,
            incoming.BindingIndex,
            &render::ShaderBindingDesc::BindingIndex);
        auto sameName = std::ranges::find(
            destination.Bindings,
            incoming.Name,
            &render::ShaderBindingDesc::Name);
        if (sameLocation == destination.Bindings.end() && sameName == destination.Bindings.end()) {
            destination.Bindings.emplace_back(incoming);
            const render::ShaderBindingGroupInterfaceDesc added{
                .GroupIndex = source.GroupIndex,
                .Bindings = {incoming}};
            AppendOrigins(added, context, origins, fieldOrigins);
            continue;
        }
        if (sameLocation == destination.Bindings.end() || sameName == destination.Bindings.end() ||
            sameLocation != sameName) {
            const render::ShaderBindingDesc* conflicting =
                sameLocation != destination.Bindings.end() ? &*sameLocation : &*sameName;
            const UserBindingOrigin* origin = FindOrigin(
                origins,
                source.GroupIndex,
                conflicting->BindingIndex);
            return UserGroupMergeResult{
                .Compatible = false,
                .Binding = incoming.BindingIndex,
                .RelatedContext = origin != nullptr
                                      ? std::optional<render::ShaderDiagnosticContext>{origin->Context}
                                      : std::nullopt,
                .Detail = "binding name and location do not identify the same user parameter"};
        }
        if (incoming.Kind == render::ShaderBindingKind::ConstantBuffer &&
            sameLocation->Kind == render::ShaderBindingKind::ConstantBuffer) {
            UserGroupMergeResult merge = MergeConstantBuffer(
                *sameLocation,
                incoming,
                source.GroupIndex,
                context,
                fieldOrigins);
            if (!merge.Compatible) {
                if (!merge.RelatedContext.has_value()) {
                    if (const UserBindingOrigin* origin = FindOrigin(
                            origins,
                            source.GroupIndex,
                            sameLocation->BindingIndex)) {
                        merge.RelatedContext = origin->Context;
                    }
                }
                return merge;
            }
            continue;
        }
        if (!render::AreShaderBindingsAbiCompatible(*sameLocation, incoming)) {
            const UserBindingOrigin* origin = FindOrigin(
                origins,
                source.GroupIndex,
                sameLocation->BindingIndex);
            return UserGroupMergeResult{
                .Compatible = false,
                .Binding = incoming.BindingIndex,
                .RelatedContext = origin != nullptr
                                      ? std::optional<render::ShaderDiagnosticContext>{origin->Context}
                                      : std::nullopt,
                .Detail = "resource binding ABI is incompatible"};
        }
        sameLocation->Stages |= incoming.Stages;
    }
    std::ranges::sort(destination.Bindings, {}, &render::ShaderBindingDesc::BindingIndex);
    return {};
}

bool CanPreserveBindingValue(
    const render::ShaderBindingDesc& previous,
    const render::ShaderBindingDesc& next) noexcept {
    if (previous.Name != next.Name) return false;
    if (previous.Kind != render::ShaderBindingKind::ConstantBuffer ||
        next.Kind != render::ShaderBindingKind::ConstantBuffer) {
        return render::AreShaderBindingsAbiCompatible(previous, next);
    }
    if (!render::IsShaderBindingAbiProjectionOf(previous, next) &&
        !render::IsShaderBindingAbiProjectionOf(next, previous)) {
        return false;
    }
    if (!previous.Buffer.has_value() || !next.Buffer.has_value()) return false;
    for (const render::ShaderInterfaceFieldDesc& oldField : previous.Buffer->Fields) {
        for (const render::ShaderInterfaceFieldDesc& newField : next.Buffer->Fields) {
            if (!RangesOverlap(
                    oldField.Offset,
                    oldField.Size,
                    newField.Offset,
                    newField.Size)) {
                continue;
            }
            if (oldField.Name != newField.Name || oldField.Offset != newField.Offset ||
                oldField.Size != newField.Size || oldField.Type != newField.Type) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

bool ShaderParameterLayout::IsValid() const noexcept {
    size_t expectedBindingCount = 0;
    for (const render::ShaderBindingGroupInterfaceDesc& group : _groups) {
        expectedBindingCount += group.Bindings.size();
    }
    if (expectedBindingCount != _bindings.size()) return false;
    for (size_t i = 0; i < _bindings.size(); ++i) {
        const ShaderParameterBindingDesc& binding = _bindings[i];
        const auto group = std::ranges::find_if(_groups, [&](const auto& value) {
            return value.GroupIndex == binding.Location.Group;
        });
        if (group == _groups.end()) return false;
        const auto source = std::ranges::find_if(group->Bindings, [&](const auto& value) {
            return value.BindingIndex == binding.Location.Binding;
        });
        if (source == group->Bindings.end() || *source != binding.Interface ||
            (source->Kind == render::ShaderBindingKind::ConstantBuffer && source->Count != 1) ||
            source->Kind == render::ShaderBindingKind::AccelerationStructure) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (_bindings[j].Location == binding.Location) return false;
        }
        for (size_t j = 0; j < binding.Fields.size(); ++j) {
            const ShaderParameterFieldDesc& field = binding.Fields[j];
            if (field.Name.empty() || field.QualifiedName.empty() || field.Size == 0 ||
                !binding.Interface.Buffer.has_value() ||
                field.Offset >= binding.Interface.Buffer->ByteSize ||
                field.Size > binding.Interface.Buffer->ByteSize - field.Offset) {
                return false;
            }
            for (size_t k = 0; k < j; ++k) {
                if (binding.Fields[k].Name == field.Name ||
                    binding.Fields[k].QualifiedName == field.QualifiedName) {
                    return false;
                }
            }
        }
    }
    return _hash != render::ShaderHash{};
}

Nullable<const ShaderParameterBindingDesc*> ShaderParameterLayout::FindBinding(
    ShaderParameterLocation location) const noexcept {
    const auto it = std::ranges::find(_bindings, location, &ShaderParameterBindingDesc::Location);
    return it == _bindings.end() ? nullptr : &*it;
}

Nullable<const ShaderParameterBindingDesc*> ShaderParameterLayout::FindBinding(
    std::string_view name) const noexcept {
    const ShaderParameterBindingDesc* result = nullptr;
    for (const ShaderParameterBindingDesc& binding : _bindings) {
        const string qualified = fmt::format("group{}.{}", binding.Location.Group, binding.Interface.Name);
        if (binding.Interface.Name != name && qualified != name) continue;
        if (result != nullptr) return nullptr;
        result = &binding;
    }
    return result;
}

Nullable<const ShaderParameterFieldDesc*> ShaderParameterLayout::FindField(
    ShaderParameterLocation location,
    std::string_view name) const noexcept {
    auto binding = FindBinding(location);
    if (!binding.HasValue()) return nullptr;
    const ShaderParameterFieldDesc* result = nullptr;
    for (const ShaderParameterFieldDesc& field : binding.Get()->Fields) {
        if (field.Name != name && field.QualifiedName != name && LeafName(field.Name) != name) continue;
        if (result != nullptr) return nullptr;
        result = &field;
    }
    return result;
}

Nullable<const ShaderParameterFieldDesc*> ShaderParameterLayout::FindField(
    std::string_view name) const noexcept {
    const ShaderParameterFieldDesc* result = nullptr;
    for (const ShaderParameterBindingDesc& binding : _bindings) {
        for (const ShaderParameterFieldDesc& field : binding.Fields) {
            if (field.Name != name && field.QualifiedName != name && LeafName(field.Name) != name) continue;
            if (result != nullptr) return nullptr;
            result = &field;
        }
    }
    return result;
}

ShaderParameterLayoutBuildResult BuildShaderParameterLayout(
    std::span<const ShaderProgramInterfaceRecord> interfaces,
    const PipelineBindingPolicy& policy,
    render::ShaderProgramKind kind) {
    ShaderParameterLayoutBuildResult result;
    if (interfaces.empty()) {
        result.Diagnostics.emplace_back(ShaderBindingDiagnostic{
            .Code = ShaderBindingDiagnosticCode::InvalidInterface,
            .Message = "parameter layout requires at least one canonical program interface",
            .ProviderName = {}});
        return result;
    }

    ShaderParameterLayout layout;
    vector<UserBindingOrigin> origins;
    vector<UserFieldOrigin> fieldOrigins;
    bool foundProgram = false;
    for (const ShaderProgramInterfaceRecord& record : interfaces) {
        const render::ShaderInterfaceDesc& interface = record.Interface;
        if (interface.Kind != kind) continue;
        foundProgram = true;
        if (!interface.PushConstants.empty()) {
            result.Diagnostics.emplace_back(ShaderBindingDiagnostic{
                .Code = ShaderBindingDiagnosticCode::InvalidInterface,
                .Message = "generic shader parameters do not support user push constants",
                .Context = record.Context,
                .ProviderName = {}});
            return result;
        }
        auto resolution = ResolveShaderBindings(interface, policy, record.Context);
        if (!resolution.Succeeded()) {
            result.Diagnostics.insert(
                result.Diagnostics.end(),
                std::make_move_iterator(resolution.Diagnostics.begin()),
                std::make_move_iterator(resolution.Diagnostics.end()));
            return result;
        }
        for (const render::ShaderBindingGroupInterfaceDesc& group : resolution.Plan->UserGroups) {
            render::ShaderBindingGroupInterfaceDesc normalizedGroup = group;
            NormalizeUserGroup(normalizedGroup);
            const auto existing = std::ranges::find_if(layout._groups, [&](const auto& value) {
                return value.GroupIndex == normalizedGroup.GroupIndex;
            });
            if (existing == layout._groups.end()) {
                layout._groups.emplace_back(std::move(normalizedGroup));
                AppendOrigins(layout._groups.back(), record.Context, origins, fieldOrigins);
            } else {
                UserGroupMergeResult merge = MergeUserGroup(
                    *existing,
                    normalizedGroup,
                    record.Context,
                    origins,
                    fieldOrigins);
                if (merge.Compatible) continue;
                render::ShaderDiagnosticContext context = record.Context;
                context.Group = normalizedGroup.GroupIndex;
                context.Binding = merge.Binding;
                result.Diagnostics.emplace_back(ShaderBindingDiagnostic{
                    .Code = ShaderBindingDiagnosticCode::InvalidInterface,
                    .Message = fmt::format(
                        "user-owned binding group {} has conflicting bindings across passes or variants: {}",
                        normalizedGroup.GroupIndex,
                        merge.Detail.empty() ? "incompatible layout" : merge.Detail),
                    .Context = std::move(context),
                    .RelatedContext = std::move(merge.RelatedContext),
                    .ProviderName = {}});
                return result;
            }
        }
    }
    if (!foundProgram) {
        result.Diagnostics.emplace_back(ShaderBindingDiagnostic{
            .Code = ShaderBindingDiagnosticCode::InvalidInterface,
            .Message = "shader binary has no baked program interface of the requested kind",
            .ProviderName = {}});
        return result;
    }

    std::ranges::sort(layout._groups, {}, &render::ShaderBindingGroupInterfaceDesc::GroupIndex);
    for (const render::ShaderBindingGroupInterfaceDesc& group : layout._groups) {
        for (const render::ShaderBindingDesc& binding : group.Bindings) {
            if ((binding.Kind == render::ShaderBindingKind::ConstantBuffer && binding.Count != 1) ||
                binding.Kind == render::ShaderBindingKind::AccelerationStructure) {
                result.Diagnostics.emplace_back(ShaderBindingDiagnostic{
                    .Code = ShaderBindingDiagnosticCode::InvalidInterface,
                    .Message = fmt::format(
                        "user binding group {} binding {} uses an unsupported parameter-set shape",
                        group.GroupIndex,
                        binding.BindingIndex),
                    .Context = [&] {
                        render::ShaderDiagnosticContext context;
                        if (const UserBindingOrigin* origin = FindOrigin(
                                origins,
                                group.GroupIndex,
                                binding.BindingIndex)) {
                            context = origin->Context;
                        }
                        context.Group = group.GroupIndex;
                        context.Binding = binding.BindingIndex;
                        return context;
                    }(),
                    .ProviderName = {}});
                return result;
            }
            ShaderParameterBindingDesc parameter{
                .Location = {.Group = group.GroupIndex, .Binding = binding.BindingIndex},
                .Interface = binding,
                .Fields = {}};
            if (binding.Buffer.has_value()) {
                AppendFields(binding.Buffer->Fields, binding.Name, {}, parameter.Fields);
            }
            layout._bindings.emplace_back(std::move(parameter));
        }
    }
    layout._hash = HashParameterGroups(layout._groups, kind);
    if (!layout.IsValid()) {
        result.Diagnostics.emplace_back(ShaderBindingDiagnostic{
            .Code = ShaderBindingDiagnosticCode::InvalidInterface,
            .Message = "resolved user parameter layout failed structural validation",
            .ProviderName = {}});
        return result;
    }
    result.Layout = std::move(layout);
    return result;
}

ShaderParameterLayoutBuildResult BuildShaderParameterLayout(
    std::span<const render::ShaderInterfaceDesc> interfaces,
    const PipelineBindingPolicy& policy,
    render::ShaderProgramKind kind) {
    vector<ShaderProgramInterfaceRecord> records;
    records.reserve(interfaces.size());
    for (const render::ShaderInterfaceDesc& interface : interfaces) {
        records.emplace_back(ShaderProgramInterfaceRecord{.Interface = interface});
    }
    return BuildShaderParameterLayout(records, policy, kind);
}

ShaderParameterLayoutBuildResult BuildShaderParameterLayout(
    const render::ShaderBinary& binary,
    const PipelineBindingPolicy& policy,
    render::ShaderProgramKind kind) {
    if (!binary.IsValid()) {
        ShaderParameterLayoutBuildResult result;
        result.Diagnostics.emplace_back(ShaderBindingDiagnostic{
            .Code = ShaderBindingDiagnosticCode::InvalidInterface,
            .Message = "parameter layout requires a structurally valid shader binary",
            .ProviderName = {}});
        return result;
    }
    vector<ShaderProgramInterfaceRecord> interfaces;
    vector<uint32_t> indices;
    for (const render::ShaderProgramVariantArtifact& program : binary.ProgramVariants) {
        if (binary.ProgramInterfaces[program.InterfaceIndex].Kind != kind ||
            std::ranges::find(indices, program.InterfaceIndex) != indices.end()) {
            continue;
        }
        indices.emplace_back(program.InterfaceIndex);
        interfaces.emplace_back(ShaderProgramInterfaceRecord{
            .Interface = binary.ProgramInterfaces[program.InterfaceIndex],
            .Context = render::ShaderDiagnosticContext{
                .Target = program.Target,
                .PassIndex = program.PassIndex,
                .VariantDefines = program.Defines}});
    }
    return BuildShaderParameterLayout(interfaces, policy, kind);
}

bool ShaderParameterSet::Reset(
    const ShaderParameterLayout& layout,
    bool preserveCompatibleValues) noexcept {
    if (!layout.IsValid()) return false;
    vector<ShaderParameterBindingValue> values;
    values.reserve(layout.Bindings().size());
    for (const ShaderParameterBindingDesc& binding : layout.Bindings()) {
        ShaderParameterBindingValue value{
            .Location = binding.Location,
            .Kind = binding.Interface.Kind,
            .ConstantData = {},
            .Resources = {}};
        if (binding.Interface.Kind == render::ShaderBindingKind::ConstantBuffer) {
            value.ConstantData.resize(binding.Interface.Buffer->ByteSize, byte{0});
        } else if (binding.Interface.Count != 0) {
            value.Resources.resize(binding.Interface.Count);
            if (binding.Interface.Kind == render::ShaderBindingKind::Sampler) {
                for (ShaderResourceParameterValue& resource : value.Resources) {
                    resource = render::SamplerDescriptor{};
                }
            }
        }
        values.emplace_back(std::move(value));
    }
    if (preserveCompatibleValues && _layout.IsValid()) {
        for (size_t i = 0; i < layout.Bindings().size(); ++i) {
            const ShaderParameterBindingDesc& nextBinding = layout.Bindings()[i];
            auto previousBinding = _layout.FindBinding(nextBinding.Location);
            ShaderParameterBindingValue* previousValue = FindValue(nextBinding.Location);
            if (!previousBinding.HasValue() || previousValue == nullptr ||
                !CanPreserveBindingValue(
                    previousBinding.Get()->Interface,
                    nextBinding.Interface)) {
                continue;
            }
            if (nextBinding.Interface.Kind == render::ShaderBindingKind::ConstantBuffer) {
                const size_t copySize = std::min(
                    values[i].ConstantData.size(),
                    previousValue->ConstantData.size());
                std::ranges::copy_n(
                    previousValue->ConstantData.begin(),
                    copySize,
                    values[i].ConstantData.begin());
            } else {
                values[i].Resources = previousValue->Resources;
            }
        }
    }
    _layout = layout;
    _values = std::move(values);
    ++_revision;
    return true;
}

bool ShaderParameterSet::IsComplete() const noexcept {
    if (!_layout.IsValid() || _values.size() != _layout.Bindings().size()) return false;
    for (size_t i = 0; i < _values.size(); ++i) {
        const ShaderParameterBindingDesc& binding = _layout.Bindings()[i];
        const ShaderParameterBindingValue& value = _values[i];
        if (value.Location != binding.Location || value.Kind != binding.Interface.Kind) return false;
        if (binding.Interface.Kind == render::ShaderBindingKind::ConstantBuffer) {
            if (!binding.Interface.Buffer.has_value() ||
                value.ConstantData.size() != binding.Interface.Buffer->ByteSize) {
                return false;
            }
            continue;
        }
        if (binding.Interface.Count != 0 && value.Resources.size() != binding.Interface.Count) return false;
        for (const ShaderResourceParameterValue& resource : value.Resources) {
            if (std::holds_alternative<std::monostate>(resource)) return false;
            if (binding.Interface.Kind == render::ShaderBindingKind::SampledTexture ||
                binding.Interface.Kind == render::ShaderBindingKind::StorageTexture) {
                const auto* texture = std::get_if<ShaderTextureParameterValue>(&resource);
                if (texture == nullptr || !IsTextureCompatible(binding.Interface, *texture)) {
                    return false;
                }
            } else if (binding.Interface.Kind == render::ShaderBindingKind::Sampler) {
                if (!std::holds_alternative<render::SamplerDescriptor>(resource)) return false;
            } else if (IsBufferKind(binding.Interface.Kind)) {
                const auto* buffer = std::get_if<ShaderBufferParameterValue>(&resource);
                if (buffer == nullptr || !buffer->Buffer.HasValue() ||
                    !IsBufferCompatible(binding.Interface, *buffer->Buffer.Get(), buffer->Range)) {
                    return false;
                }
            } else {
                return false;
            }
        }
    }
    return true;
}

bool ShaderParameterSet::IsCompleteFor(
    const ResolvedShaderBindingPlan& plan) const noexcept {
    if (!_layout.IsValid() || _values.size() != _layout.Bindings().size()) return false;
    for (const render::ShaderBindingGroupInterfaceDesc& group : plan.UserGroups) {
        for (const render::ShaderBindingDesc& actual : group.Bindings) {
            const ShaderParameterLocation location{
                .Group = group.GroupIndex,
                .Binding = actual.BindingIndex};
            auto binding = _layout.FindBinding(location);
            const ShaderParameterBindingValue* value = FindValue(location);
            if (!binding.HasValue() || value == nullptr ||
                binding.Get()->Interface.Name != actual.Name ||
                !render::IsShaderBindingAbiProjectionOf(
                    actual,
                    binding.Get()->Interface)) {
                return false;
            }

            if (actual.Kind == render::ShaderBindingKind::ConstantBuffer) {
                if (!actual.Buffer.has_value() ||
                    !binding.Get()->Interface.Buffer.has_value() ||
                    value->ConstantData.size() !=
                        binding.Get()->Interface.Buffer->ByteSize ||
                    value->ConstantData.size() < actual.Buffer->ByteSize) {
                    return false;
                }
                continue;
            }
            if (actual.Count != 0 && value->Resources.size() != actual.Count) return false;
            for (const ShaderResourceParameterValue& resource : value->Resources) {
                if (std::holds_alternative<std::monostate>(resource)) return false;
                if (actual.Kind == render::ShaderBindingKind::SampledTexture ||
                    actual.Kind == render::ShaderBindingKind::StorageTexture) {
                    const auto* texture = std::get_if<ShaderTextureParameterValue>(&resource);
                    if (texture == nullptr || !IsTextureCompatible(actual, *texture)) {
                        return false;
                    }
                } else if (actual.Kind == render::ShaderBindingKind::Sampler) {
                    if (!std::holds_alternative<render::SamplerDescriptor>(resource)) return false;
                } else if (IsBufferKind(actual.Kind)) {
                    const auto* buffer = std::get_if<ShaderBufferParameterValue>(&resource);
                    if (buffer == nullptr || !buffer->Buffer.HasValue() ||
                        !IsBufferCompatible(actual, *buffer->Buffer.Get(), buffer->Range)) {
                        return false;
                    }
                } else {
                    return false;
                }
            }
        }
    }
    return true;
}

ShaderParameterBindingValue* ShaderParameterSet::FindValue(
    ShaderParameterLocation location) noexcept {
    const auto it = std::ranges::find(_values, location, &ShaderParameterBindingValue::Location);
    return it == _values.end() ? nullptr : &*it;
}

const ShaderParameterBindingValue* ShaderParameterSet::FindValue(
    ShaderParameterLocation location) const noexcept {
    const auto it = std::ranges::find(_values, location, &ShaderParameterBindingValue::Location);
    return it == _values.end() ? nullptr : &*it;
}

ShaderParameterSet::FieldTarget ShaderParameterSet::FindField(std::string_view name) noexcept {
    auto field = _layout.FindField(name);
    if (!field.HasValue()) return {};
    for (const ShaderParameterBindingDesc& binding : _layout.Bindings()) {
        const auto member = std::ranges::find(binding.Fields, *field.Get());
        if (member != binding.Fields.end()) {
            return {.Value = FindValue(binding.Location), .Field = field.Get()};
        }
    }
    return {};
}

ShaderParameterSet::FieldTarget ShaderParameterSet::FindField(
    ShaderParameterLocation location,
    std::string_view name) noexcept {
    auto field = _layout.FindField(location, name);
    return field.HasValue()
               ? FieldTarget{.Value = FindValue(location), .Field = field.Get()}
               : FieldTarget{};
}

bool ShaderParameterSet::SetValue(
    std::string_view name,
    render::ShaderScalarType scalar,
    uint32_t columns,
    std::span<const byte> data,
    uint32_t arrayIndex) noexcept {
    return SetValue(FindField(name), scalar, columns, data, arrayIndex);
}

bool ShaderParameterSet::SetValue(
    ShaderParameterLocation location,
    std::string_view field,
    render::ShaderScalarType scalar,
    uint32_t columns,
    std::span<const byte> data,
    uint32_t arrayIndex) noexcept {
    return SetValue(FindField(location, field), scalar, columns, data, arrayIndex);
}

bool ShaderParameterSet::SetValue(
    FieldTarget target,
    render::ShaderScalarType scalar,
    uint32_t columns,
    std::span<const byte> data,
    uint32_t arrayIndex) noexcept {
    const uint32_t scalarSize = ScalarSize(scalar);
    if (target.Value == nullptr || target.Field == nullptr || scalarSize == 0 || columns == 0 ||
        target.Value->Kind != render::ShaderBindingKind::ConstantBuffer ||
        target.Field->Type.Scalar != scalar || target.Field->Type.Rows != 1 ||
        target.Field->Type.Columns != columns || arrayIndex >= target.Field->Type.ArrayCount ||
        data.size() != static_cast<size_t>(scalarSize) * columns) {
        return false;
    }
    const uint32_t arrayOffset = target.Field->Type.ArrayCount > 1
                                     ? arrayIndex * target.Field->Type.ArrayStride
                                     : 0;
    const uint64_t offset = static_cast<uint64_t>(target.Field->Offset) + arrayOffset;
    if (offset > target.Value->ConstantData.size() || data.size() > target.Value->ConstantData.size() - offset) {
        return false;
    }
    std::memcpy(target.Value->ConstantData.data() + offset, data.data(), data.size());
    ++_revision;
    return true;
}

bool ShaderParameterSet::SetFloat(std::string_view name, float value, uint32_t arrayIndex) noexcept {
    return SetValue(name, render::ShaderScalarType::Float32, 1, std::as_bytes(std::span{&value, 1}), arrayIndex);
}

bool ShaderParameterSet::SetFloat(
    ShaderParameterLocation location,
    std::string_view field,
    float value,
    uint32_t arrayIndex) noexcept {
    return SetValue(
        location,
        field,
        render::ShaderScalarType::Float32,
        1,
        std::as_bytes(std::span{&value, 1}),
        arrayIndex);
}

bool ShaderParameterSet::SetInt(std::string_view name, int32_t value, uint32_t arrayIndex) noexcept {
    return SetValue(name, render::ShaderScalarType::Int32, 1, std::as_bytes(std::span{&value, 1}), arrayIndex);
}

bool ShaderParameterSet::SetInt(
    ShaderParameterLocation location,
    std::string_view field,
    int32_t value,
    uint32_t arrayIndex) noexcept {
    return SetValue(
        location,
        field,
        render::ShaderScalarType::Int32,
        1,
        std::as_bytes(std::span{&value, 1}),
        arrayIndex);
}

bool ShaderParameterSet::SetUInt(std::string_view name, uint32_t value, uint32_t arrayIndex) noexcept {
    return SetValue(name, render::ShaderScalarType::UInt32, 1, std::as_bytes(std::span{&value, 1}), arrayIndex);
}

bool ShaderParameterSet::SetUInt(
    ShaderParameterLocation location,
    std::string_view field,
    uint32_t value,
    uint32_t arrayIndex) noexcept {
    return SetValue(
        location,
        field,
        render::ShaderScalarType::UInt32,
        1,
        std::as_bytes(std::span{&value, 1}),
        arrayIndex);
}

bool ShaderParameterSet::SetBool(std::string_view name, bool value, uint32_t arrayIndex) noexcept {
    const uint32_t encoded = value ? 1u : 0u;
    return SetValue(name, render::ShaderScalarType::Bool, 1, std::as_bytes(std::span{&encoded, 1}), arrayIndex);
}

bool ShaderParameterSet::SetBool(
    ShaderParameterLocation location,
    std::string_view field,
    bool value,
    uint32_t arrayIndex) noexcept {
    const uint32_t encoded = value ? 1u : 0u;
    return SetValue(
        location,
        field,
        render::ShaderScalarType::Bool,
        1,
        std::as_bytes(std::span{&encoded, 1}),
        arrayIndex);
}

bool ShaderParameterSet::SetVector(
    std::string_view name,
    const Eigen::Vector4f& value,
    uint32_t arrayIndex) noexcept {
    return SetValue(
        name,
        render::ShaderScalarType::Float32,
        4,
        std::as_bytes(std::span{value.data(), 4}),
        arrayIndex);
}

bool ShaderParameterSet::SetVector(
    ShaderParameterLocation location,
    std::string_view field,
    const Eigen::Vector4f& value,
    uint32_t arrayIndex) noexcept {
    return SetValue(
        location,
        field,
        render::ShaderScalarType::Float32,
        4,
        std::as_bytes(std::span{value.data(), 4}),
        arrayIndex);
}

bool ShaderParameterSet::SetMatrix(
    std::string_view name,
    std::span<const float> rowMajorValues,
    uint32_t rows,
    uint32_t columns,
    uint32_t arrayIndex) noexcept {
    return SetMatrix(FindField(name), rowMajorValues, rows, columns, arrayIndex);
}

bool ShaderParameterSet::SetMatrix(
    ShaderParameterLocation location,
    std::string_view field,
    std::span<const float> rowMajorValues,
    uint32_t rows,
    uint32_t columns,
    uint32_t arrayIndex) noexcept {
    return SetMatrix(
        FindField(location, field),
        rowMajorValues,
        rows,
        columns,
        arrayIndex);
}

bool ShaderParameterSet::SetMatrix(
    FieldTarget target,
    std::span<const float> rowMajorValues,
    uint32_t rows,
    uint32_t columns,
    uint32_t arrayIndex) noexcept {
    if (target.Value == nullptr || target.Field == nullptr ||
        target.Value->Kind != render::ShaderBindingKind::ConstantBuffer ||
        target.Field->Type.Scalar != render::ShaderScalarType::Float32 ||
        target.Field->Type.Rows != rows || target.Field->Type.Columns != columns ||
        rows <= 1 || columns <= 1 || target.Field->Type.MatrixStride == 0 ||
        arrayIndex >= target.Field->Type.ArrayCount || rowMajorValues.size() != rows * columns) {
        return false;
    }
    const uint32_t elementSize = target.Field->Type.ArrayCount > 1
                                     ? target.Field->Type.ArrayStride
                                     : target.Field->Size;
    const uint64_t base = static_cast<uint64_t>(target.Field->Offset) +
                          (target.Field->Type.ArrayCount > 1 ? arrayIndex * elementSize : 0);
    if (base > target.Value->ConstantData.size() ||
        elementSize > target.Value->ConstantData.size() - base) {
        return false;
    }
    std::fill_n(target.Value->ConstantData.begin() + base, elementSize, byte{0});
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t column = 0; column < columns; ++column) {
            const uint32_t destination = target.Field->Type.RowMajor
                                             ? row * target.Field->Type.MatrixStride + column * sizeof(float)
                                             : column * target.Field->Type.MatrixStride + row * sizeof(float);
            if (destination > elementSize - sizeof(float)) return false;
            const float value = rowMajorValues[row * columns + column];
            std::memcpy(target.Value->ConstantData.data() + base + destination, &value, sizeof(value));
        }
    }
    ++_revision;
    return true;
}

bool ShaderParameterSet::SetConstantBuffer(
    ShaderParameterLocation location,
    std::span<const byte> data) noexcept {
    auto binding = _layout.FindBinding(location);
    ShaderParameterBindingValue* value = FindValue(location);
    if (!binding.HasValue() || value == nullptr ||
        binding.Get()->Interface.Kind != render::ShaderBindingKind::ConstantBuffer ||
        data.size() != value->ConstantData.size()) {
        return false;
    }
    std::copy(data.begin(), data.end(), value->ConstantData.begin());
    ++_revision;
    return true;
}

bool ShaderParameterSet::SetConstantBuffer(
    std::string_view name,
    std::span<const byte> data) noexcept {
    auto binding = _layout.FindBinding(name);
    return binding.HasValue() ? SetConstantBuffer(binding.Get()->Location, data) : false;
}

bool ShaderParameterSet::SetResourceValue(
    const ShaderParameterBindingDesc& binding,
    ShaderResourceParameterValue value,
    uint32_t arrayIndex) noexcept {
    ShaderParameterBindingValue* target = FindValue(binding.Location);
    if (target == nullptr || binding.Interface.Kind == render::ShaderBindingKind::ConstantBuffer) return false;
    if (binding.Interface.Count != 0 && arrayIndex >= binding.Interface.Count) return false;
    if (arrayIndex >= target->Resources.size()) {
        if (binding.Interface.Count != 0) return false;
        target->Resources.resize(static_cast<size_t>(arrayIndex) + 1);
    }
    target->Resources[arrayIndex] = std::move(value);
    ++_revision;
    return true;
}

bool ShaderParameterSet::SetTexture(
    ShaderParameterLocation location,
    StreamingAssetRef<TextureAsset> texture,
    const TextureSubViewDesc& view,
    uint32_t arrayIndex) noexcept {
    auto binding = _layout.FindBinding(location);
    if (!binding.HasValue() || !texture.IsValid() ||
        (binding.Get()->Interface.Kind != render::ShaderBindingKind::SampledTexture &&
         binding.Get()->Interface.Kind != render::ShaderBindingKind::StorageTexture)) {
        return false;
    }
    ShaderTextureParameterValue value{.Texture = std::move(texture), .View = view};
    if (value.Texture.IsReady() && !IsTextureCompatible(binding.Get()->Interface, value)) return false;
    return SetResourceValue(*binding.Get(), std::move(value), arrayIndex);
}

bool ShaderParameterSet::SetTexture(
    std::string_view name,
    StreamingAssetRef<TextureAsset> texture,
    const TextureSubViewDesc& view,
    uint32_t arrayIndex) noexcept {
    auto binding = _layout.FindBinding(name);
    return binding.HasValue()
               ? SetTexture(binding.Get()->Location, std::move(texture), view, arrayIndex)
               : false;
}

bool ShaderParameterSet::SetSampler(
    ShaderParameterLocation location,
    const render::SamplerDescriptor& sampler,
    uint32_t arrayIndex) noexcept {
    auto binding = _layout.FindBinding(location);
    if (!binding.HasValue() || binding.Get()->Interface.Kind != render::ShaderBindingKind::Sampler) return false;
    return SetResourceValue(*binding.Get(), sampler, arrayIndex);
}

bool ShaderParameterSet::SetSampler(
    std::string_view name,
    const render::SamplerDescriptor& sampler,
    uint32_t arrayIndex) noexcept {
    auto binding = _layout.FindBinding(name);
    return binding.HasValue() ? SetSampler(binding.Get()->Location, sampler, arrayIndex) : false;
}

bool ShaderParameterSet::SetBuffer(
    ShaderParameterLocation location,
    Nullable<render::Buffer*> buffer,
    render::BufferRange range,
    uint32_t arrayIndex) noexcept {
    auto binding = _layout.FindBinding(location);
    if (!binding.HasValue() || !buffer.HasValue() ||
        !IsBufferKind(binding.Get()->Interface.Kind) ||
        !IsBufferCompatible(binding.Get()->Interface, *buffer.Get(), range)) {
        return false;
    }
    return SetResourceValue(
        *binding.Get(),
        ShaderBufferParameterValue{.Buffer = buffer, .Range = range},
        arrayIndex);
}

bool ShaderParameterSet::SetBuffer(
    std::string_view name,
    Nullable<render::Buffer*> buffer,
    render::BufferRange range,
    uint32_t arrayIndex) noexcept {
    auto binding = _layout.FindBinding(name);
    return binding.HasValue() ? SetBuffer(binding.Get()->Location, buffer, range, arrayIndex) : false;
}

bool ShaderParameterSet::ClearResource(
    ShaderParameterLocation location,
    uint32_t arrayIndex) noexcept {
    auto binding = _layout.FindBinding(location);
    if (!binding.HasValue() || binding.Get()->Interface.Kind == render::ShaderBindingKind::ConstantBuffer) return false;
    return SetResourceValue(*binding.Get(), std::monostate{}, arrayIndex);
}

bool ShaderParameterSet::ClearResource(std::string_view name, uint32_t arrayIndex) noexcept {
    auto binding = _layout.FindBinding(name);
    return binding.HasValue() ? ClearResource(binding.Get()->Location, arrayIndex) : false;
}

}  // namespace radray
