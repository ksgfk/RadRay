#include <radray/runtime/render_framework/material_constant_binder.h>

#include <algorithm>
#include <cstring>
#include <variant>

#include <radray/logger.h>
#include <radray/render/shader/hlsl.h>
#include <radray/render/shader/spirv.h>

namespace radray {

namespace {

// 判断一个块名是否在保留 (系统) 块名集里。
bool IsReserved(std::string_view name, std::span<const std::string_view> reserved) noexcept {
    for (std::string_view r : reserved) {
        if (!r.empty() && r == name) {
            return true;
        }
    }
    return false;
}

bool IsCBufferBinding(
    render::PipelineLayout* layout,
    const render::ShaderBindingLocation& location) noexcept {
    if (layout == nullptr) {
        return false;
    }
    for (const render::BindingGroupLayout& group : layout->GetBindingGroupLayouts()) {
        if (group.GroupIndex != location.Group) {
            continue;
        }
        const auto entry = std::ranges::find_if(
            group.Entries,
            [&location](const render::BindingGroupLayoutEntry& binding) noexcept {
                return binding.Binding == location.Binding;
            });
        return entry != group.Entries.end() &&
               entry->Parameter.Kind == render::ShaderParameterKind::Resource &&
               entry->Parameter.Type == render::ResourceBindType::CBuffer;
    }
    return false;
}

// 从 HLSL 反射把 cbuffer 块 (含字段偏移) 提取进 out。
void ExtractHlslBlocks(
    const render::HlslShaderDesc& hlsl,
    render::PipelineLayout* layout,
    vector<string>& seenNames,
    auto&& appendBlock) noexcept {
    for (const render::HlslShaderBufferDesc& cb : hlsl.ConstantBuffers) {
        // 仅处理常规 cbuffer (排除 tbuffer / resource bind info 等)。
        if (cb.Type != render::HlslCBufferType::CBUFFER) {
            continue;
        }
        const auto resource = std::ranges::find_if(
            hlsl.BoundResources,
            [&cb](const render::HlslInputBindDesc& binding) noexcept {
                return binding.Name == cb.Name &&
                       binding.Type == render::HlslShaderInputType::CBUFFER;
            });
        if (resource == hlsl.BoundResources.end()) {
            continue;
        }
        const render::ShaderBindingLocation location{
            .Group = resource->VkSet.value_or(resource->Space),
            .Binding = resource->VkBinding.value_or(resource->BindPoint)};
        if (!IsCBufferBinding(layout, location)) {
            continue;  // 块未出现在合并 binding layout (可能被优化掉)。
        }
        if (std::ranges::find(seenNames, cb.Name) != seenNames.end()) {
            continue;
        }
        seenNames.emplace_back(cb.Name);
        // 提取字段偏移。
        vector<std::pair<string, std::pair<uint32_t, uint32_t>>> fields;
        fields.reserve(cb.Variables.size());
        for (const size_t varIdx : cb.Variables) {
            if (varIdx >= hlsl.Variables.size()) {
                continue;
            }
            const render::HlslShaderVariableDesc& var = hlsl.Variables[varIdx];
            fields.emplace_back(var.Name, std::make_pair(var.StartOffset, var.Size));
        }
        appendBlock(cb.Name, cb.Size, location, fields);
    }
}

// 从 SPIR-V 反射把 uniform buffer 块 (含字段偏移) 提取进 out。
void ExtractSpirvBlocks(
    const render::SpirvShaderDesc& spv,
    render::PipelineLayout* layout,
    vector<string>& seenNames,
    auto&& appendBlock) noexcept {
    auto emitBlock = [&](std::string_view name,
                         uint32_t typeIndex,
                         uint32_t blockSize,
                         render::ShaderBindingLocation location) {
        if (!IsCBufferBinding(layout, location)) {
            return;
        }
        if (std::ranges::find(seenNames, name) != seenNames.end()) {
            return;
        }
        seenNames.emplace_back(name);
        vector<std::pair<string, std::pair<uint32_t, uint32_t>>> fields;
        if (typeIndex < spv.Types.size()) {
            const render::SpirvTypeInfo& type = spv.Types[typeIndex];
            fields.reserve(type.Members.size());
            for (const render::SpirvTypeMember& m : type.Members) {
                fields.emplace_back(m.Name, std::make_pair(m.Offset, m.Size));
            }
        }
        appendBlock(name, blockSize, location, fields);
    };
    for (const render::SpirvResourceBinding& res : spv.ResourceBindings) {
        if (res.Kind == render::SpirvResourceKind::UniformBuffer) {
            emitBlock(
                res.Name,
                res.TypeIndex,
                res.UniformBufferSize,
                render::ShaderBindingLocation{.Group = res.Set, .Binding = res.Binding});
        }
    }
}

}  // namespace

void MaterialConstantBinder::AppendBlocksFromReflection(
    render::Shader* shader,
    render::PipelineLayout* layout,
    vector<BlockLayout>& out) noexcept {
    if (shader == nullptr || layout == nullptr) {
        return;
    }
    auto reflOpt = shader->GetReflection();
    if (!reflOpt.HasValue() || reflOpt.Get() == nullptr) {
        return;
    }
    const render::ShaderReflectionDesc& refl = *reflOpt.Get();

    vector<string> seenNames;
    seenNames.reserve(out.size());
    for (const BlockLayout& b : out) {
        seenNames.emplace_back(b.Name);
    }

    auto appendBlock = [&](std::string_view name,
                           uint32_t size,
                           render::ShaderBindingLocation location,
                           const vector<std::pair<string, std::pair<uint32_t, uint32_t>>>& fields) {
        BlockLayout block{};
        block.Name.assign(name.begin(), name.end());
        block.Size = size;
        block.Location = location;
        block.Fields.reserve(fields.size());
        for (const auto& [fname, off] : fields) {
            block.Fields.emplace_back(FieldLayout{fname, off.first, off.second});
        }
        out.emplace_back(std::move(block));
    };

    if (const auto* hlsl = std::get_if<render::HlslShaderDesc>(&refl)) {
        ExtractHlslBlocks(*hlsl, layout, seenNames, appendBlock);
    } else if (const auto* spv = std::get_if<render::SpirvShaderDesc>(&refl)) {
        ExtractSpirvBlocks(*spv, layout, seenNames, appendBlock);
    }
}

const vector<MaterialConstantBinder::BlockLayout>& MaterialConstantBinder::GetOrExtract(
    const CompiledShaderVariant& variant) noexcept {
    auto it = _cache.find(&variant);
    if (it != _cache.end()) {
        return it->second;
    }
    vector<BlockLayout> blocks;
    if (variant.Layout != nullptr) {
        AppendBlocksFromReflection(variant.VS, variant.Layout, blocks);
        AppendBlocksFromReflection(variant.PS, variant.Layout, blocks);
        AppendBlocksFromReflection(variant.CS, variant.Layout, blocks);
    }
    auto [pos, _] = _cache.emplace(&variant, std::move(blocks));
    return pos->second;
}


uint32_t MaterialConstantBinder::Bind(
    const CompiledShaderVariant& variant,
    render::BindingGroup& group,
    MaterialConstantPool& pool,
    std::span<const MaterialConstantValue> values,
    std::span<const std::string_view> reservedBlockNames,
    vector<MaterialConstantPool::Allocation>* allocations) noexcept {
    if (variant.Layout == nullptr) {
        return 0;
    }
    const vector<BlockLayout>& blocks = GetOrExtract(variant);
    uint32_t bound = 0;
    for (const BlockLayout& block : blocks) {
        if (!block.Location.has_value() || block.Location->Group != group.GetGroupIndex() ||
            IsReserved(block.Name, reservedBlockNames) || block.Size == 0) {
            continue;
        }

        _stagingScratch.assign(block.Size, byte{0});
        bool anyHit = false;
        for (const MaterialConstantValue& value : values) {
            if (value.Bytes.empty()) {
                continue;
            }
            if (value.Name == block.Name) {
                const size_t size = std::min<size_t>(value.Bytes.size(), block.Size);
                std::memcpy(_stagingScratch.data(), value.Bytes.data(), size);
                anyHit = true;
                continue;
            }
            for (const FieldLayout& field : block.Fields) {
                if (value.Name != field.Name || field.Offset >= block.Size) {
                    continue;
                }
                const size_t size = std::min<size_t>({
                    value.Bytes.size(),
                    static_cast<size_t>(field.Size),
                    static_cast<size_t>(block.Size - field.Offset)});
                std::memcpy(_stagingScratch.data() + field.Offset, value.Bytes.data(), size);
                anyHit = true;
                break;
            }
        }
        if (!anyHit) {
            continue;
        }

        auto allocation = pool.Allocate(block.Size);
        if (allocation.Target == nullptr || allocation.Mapped == nullptr) {
            RADRAY_ERR_LOG("MaterialConstantBinder: cbuffer allocation failed for block '{}'", block.Name);
            continue;
        }
        std::memcpy(allocation.Mapped, _stagingScratch.data(), block.Size);
        render::BufferBindingDescriptor descriptor{};
        descriptor.Target = allocation.Target;
        descriptor.Range = render::BufferRange{.Offset = allocation.Offset, .Size = block.Size};
        descriptor.Usage = render::BufferViewUsage::CBuffer;
        if (group.SetResource(block.Location->Binding, descriptor)) {
            if (allocations != nullptr) {
                allocations->push_back(allocation);
            }
            ++bound;
        } else {
            pool.Release(allocation);
        }
    }
    pool.FlushHostWrites();
    return bound;
}

void MaterialConstantBinder::ClearCache() noexcept {
    _cache.clear();
}

}  // namespace radray
