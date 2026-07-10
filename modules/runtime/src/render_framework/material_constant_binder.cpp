#include <radray/runtime/render_framework/material_constant_binder.h>

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

// 从 HLSL 反射把 cbuffer 块 (含字段偏移) 提取进 out。
void ExtractHlslBlocks(
    const render::HlslShaderDesc& hlsl,
    render::ShaderBindingLayout* layout,
    vector<render::ShaderParameterId>& seenIds,
    auto&& appendBlock) noexcept {
    for (const render::HlslShaderBufferDesc& cb : hlsl.ConstantBuffers) {
        // 仅处理常规 cbuffer (排除 tbuffer / resource bind info 等)。
        if (cb.Type != render::HlslCBufferType::CBUFFER) {
            continue;
        }
        auto idOpt = layout->FindParameterId(cb.Name);
        if (!idOpt.has_value()) {
            continue;  // 块未出现在合并 binding layout (可能被优化掉)。
        }
        const render::ShaderParameterId id = idOpt.value();
        // 按块名 (via id) 去重: 多 stage 引用同一块只登记一次。
        bool seen = false;
        for (const render::ShaderParameterId s : seenIds) {
            if (s.Value == id.Value) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        seenIds.emplace_back(id);
        auto* param = layout->FindParameter(id).Get();
        const render::ShaderParameterKind kind =
            param != nullptr ? param->Kind : render::ShaderParameterKind::UNKNOWN;
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
        appendBlock(cb.Name, id, cb.Size, kind, fields);
    }
}

// 从 SPIR-V 反射把 uniform buffer / push constant 块 (含字段偏移) 提取进 out。
void ExtractSpirvBlocks(
    const render::SpirvShaderDesc& spv,
    render::ShaderBindingLayout* layout,
    vector<render::ShaderParameterId>& seenIds,
    auto&& appendBlock) noexcept {
    auto emitBlock = [&](std::string_view name, uint32_t typeIndex, uint32_t blockSize) {
        auto idOpt = layout->FindParameterId(name);
        if (!idOpt.has_value()) {
            return;
        }
        const render::ShaderParameterId id = idOpt.value();
        for (const render::ShaderParameterId s : seenIds) {
            if (s.Value == id.Value) {
                return;
            }
        }
        seenIds.emplace_back(id);
        auto* param = layout->FindParameter(id).Get();
        const render::ShaderParameterKind kind =
            param != nullptr ? param->Kind : render::ShaderParameterKind::UNKNOWN;
        vector<std::pair<string, std::pair<uint32_t, uint32_t>>> fields;
        if (typeIndex < spv.Types.size()) {
            const render::SpirvTypeInfo& type = spv.Types[typeIndex];
            fields.reserve(type.Members.size());
            for (const render::SpirvTypeMember& m : type.Members) {
                fields.emplace_back(m.Name, std::make_pair(m.Offset, m.Size));
            }
        }
        appendBlock(name, id, blockSize, kind, fields);
    };
    for (const render::SpirvResourceBinding& res : spv.ResourceBindings) {
        if (res.Kind == render::SpirvResourceKind::UniformBuffer) {
            emitBlock(res.Name, res.TypeIndex, res.UniformBufferSize);
        }
    }
    for (const render::SpirvPushConstantRange& pc : spv.ConstantRanges) {
        emitBlock(pc.Name, pc.TypeIndex, pc.Size);
    }
}

}  // namespace

void MaterialConstantBinder::AppendBlocksFromReflection(
    render::Shader* shader,
    render::ShaderBindingLayout* layout,
    vector<BlockLayout>& out) noexcept {
    if (shader == nullptr || layout == nullptr) {
        return;
    }
    auto reflOpt = shader->GetReflection();
    if (!reflOpt.HasValue() || reflOpt.Get() == nullptr) {
        return;
    }
    const render::ShaderReflectionDesc& refl = *reflOpt.Get();

    // 收集已登记的 id (跨 stage 去重)。
    vector<render::ShaderParameterId> seenIds;
    seenIds.reserve(out.size());
    for (const BlockLayout& b : out) {
        seenIds.emplace_back(b.Id);
    }

    auto appendBlock = [&](std::string_view name,
                           render::ShaderParameterId id,
                           uint32_t size,
                           render::ShaderParameterKind kind,
                           const vector<std::pair<string, std::pair<uint32_t, uint32_t>>>& fields) {
        BlockLayout block{};
        block.Name.assign(name.begin(), name.end());
        block.Id = id;
        block.Size = size;
        block.Kind = kind;
        block.Fields.reserve(fields.size());
        for (const auto& [fname, off] : fields) {
            block.Fields.emplace_back(FieldLayout{fname, off.first, off.second});
        }
        out.emplace_back(std::move(block));
    };

    if (const auto* hlsl = std::get_if<render::HlslShaderDesc>(&refl)) {
        ExtractHlslBlocks(*hlsl, layout, seenIds, appendBlock);
    } else if (const auto* spv = std::get_if<render::SpirvShaderDesc>(&refl)) {
        ExtractSpirvBlocks(*spv, layout, seenIds, appendBlock);
    }
}

const vector<MaterialConstantBinder::BlockLayout>& MaterialConstantBinder::GetOrExtract(
    const render::CompiledShaderVariant& variant) noexcept {
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
    const render::CompiledShaderVariant& variant,
    render::ShaderParameterTable& table,
    render::CBufferArena& arena,
    std::span<const MaterialConstantValue> values,
    std::span<const std::string_view> reservedBlockNames) noexcept {
    if (variant.Layout == nullptr) {
        return 0;
    }
    const vector<BlockLayout>& blocks = GetOrExtract(variant);
    if (blocks.empty()) {
        return 0;
    }

    uint32_t bound = 0;
    for (const BlockLayout& block : blocks) {
        if (IsReserved(block.Name, reservedBlockNames) || block.Size == 0) {
            continue;
        }

        // 打包整块: 先清零, 再叠加命中该块的值 (整块覆盖 or 字段写入)。
        _stagingScratch.assign(block.Size, byte{0});
        bool anyHit = false;

        for (const MaterialConstantValue& v : values) {
            if (v.Bytes.empty()) {
                continue;
            }
            // 1) 整块覆盖: value 名字 == 块名 (对应 SetConstantBlock / 按块名 SetVector)。
            if (v.Name == block.Name) {
                const size_t n = std::min<size_t>(v.Bytes.size(), block.Size);
                std::memcpy(_stagingScratch.data(), v.Bytes.data(), n);
                anyHit = true;
                continue;
            }
            // 2) 字段写入: value 名字 == 块内某字段名 (对应 SetFloat("_Color") 语义)。
            for (const FieldLayout& f : block.Fields) {
                if (v.Name != f.Name) {
                    continue;
                }
                if (f.Offset >= block.Size) {
                    break;
                }
                const size_t cap = block.Size - f.Offset;
                const size_t n = std::min<size_t>({v.Bytes.size(), static_cast<size_t>(f.Size), cap});
                std::memcpy(_stagingScratch.data() + f.Offset, v.Bytes.data(), n);
                anyHit = true;
                break;
            }
        }

        if (!anyHit) {
            continue;  // 该块没有任何材质值命中, 不绑定 (交由默认 / 其他来源)。
        }

        // 按块的绑定类型提交。
        if (block.Kind == render::ShaderParameterKind::Constant) {
            // push / root constant: 整块 SetBytes。
            if (table.SetBytes(block.Id, _stagingScratch.data(), block.Size)) {
                ++bound;
            }
        } else {
            // 真实 cbuffer (CBV): 分配 upload buffer, memcpy, SetResource。
            auto alloc = arena.Allocate(block.Size);
            if (alloc.Target == nullptr || alloc.Mapped == nullptr) {
                RADRAY_ERR_LOG("MaterialConstantBinder: cbuffer arena allocation failed for block '{}'", block.Name);
                continue;
            }
            std::memcpy(alloc.Mapped, _stagingScratch.data(), block.Size);
            render::BufferBindingDescriptor bbd{};
            bbd.Target = alloc.Target;
            bbd.Range = render::BufferRange{.Offset = alloc.Offset, .Size = block.Size};
            bbd.Usage = render::BufferViewUsage::CBuffer;
            if (table.SetResource(block.Id, bbd)) {
                ++bound;
            }
        }
    }
    return bound;
}

void MaterialConstantBinder::ClearCache() noexcept {
    _cache.clear();
}

}  // namespace radray
