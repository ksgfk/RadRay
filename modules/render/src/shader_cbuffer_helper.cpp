#include <radray/render/shader_cbuffer_helper.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace radray::render {

size_t ShaderCBufferStorage::Builder::AddType(std::string_view name, size_t size) noexcept {
    size_t index = _types.size();
    auto& type = _types.emplace_back();
    type.Name = string{name};
    type.SizeInBytes = size;
    type.Index = index;
    return index;
}

void ShaderCBufferStorage::Builder::AddMemberForType(size_t targetType, size_t memberType, std::string_view name, size_t offset) noexcept {
    RADRAY_ASSERT(targetType < _types.size());
    auto& type = _types[targetType];
    auto& member = type.Members.emplace_back();
    member.Name = string{name};
    member.Offset = offset;
    member.TypeIndex = memberType;
}

void ShaderCBufferStorage::Builder::AddRootType(size_t typeIndex) noexcept {
    _roots.push_back(typeIndex);
}

std::optional<ShaderCBufferStorage> ShaderCBufferStorage::Builder::Build() noexcept {
    return std::nullopt;
}

static void _BuildType(ShaderCBufferStorage::Builder& builder, std::string_view name, const HlslShaderTypeDesc* type, size_t fromTypeIndex) noexcept {
    if (type == nullptr) {
        return;
    }
    auto typeIndex = builder.AddType(type->Name, type->GetSizeInBytes());
    builder.AddMemberForType(fromTypeIndex, typeIndex, name, type->Offset);
    for (const auto& member : type->Members) {
        _BuildType(builder, member.Name, member.Type, typeIndex);
    }
}

std::optional<ShaderCBufferStorage> CreateCBufferStorage(std::span<const HlslShaderDesc*> descs) noexcept {
    MergedHlslShaderDesc desc = MergeHlslShaderDesc(descs);
    if (desc.BoundResources.empty()) {
        return std::nullopt;
    }
    ShaderCBufferStorage::Builder builder{};
    for (const auto& binding : desc.BoundResources) {
        if (binding.Type != HlslShaderInputType::CBUFFER) {
            continue;
        }
        auto cbufferDataOpt = desc.FindCBufferByName(binding.Name);
        if (!cbufferDataOpt.has_value()) {
            RADRAY_ERR_LOG("{} {}", "CreateCBufferStorage", "cannot find cbuffer data");
            return std::nullopt;
        }
        const auto& cbufferData = cbufferDataOpt.value().get();
        auto rootTypeIndex = builder.AddType(cbufferData.Name, cbufferData.Size);
        builder.AddRootType(rootTypeIndex);
        for (const auto& cbVar : cbufferData.Variables) {
            auto cbType = cbVar->Type;
            _BuildType(builder, cbVar->Name, cbType, rootTypeIndex);
        }
    }
    return builder.Build();
}

}  // namespace radray::render
