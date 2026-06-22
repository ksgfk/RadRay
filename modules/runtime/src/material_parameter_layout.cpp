#include <radray/runtime/material_parameter_layout.h>

#include <algorithm>
#include <variant>

#include <radray/logger.h>
#include <radray/render/shader/hlsl.h>
#include <radray/render/shader/spirv.h>

namespace radray {

namespace {

// 解析结果的中转载体(后端无关)。两条解析路径各自填充它,
// 再由 CreateFromReflection 落到 MaterialParameterLayout 的私有成员。
struct ParsedLayout {
    bool HasConstantBuffer{false};
    string CBufferName{};
    uint32_t CBufferSize{0};
    vector<MaterialParameterLayout::Field> Fields{};
    vector<MaterialParameterLayout::ResourceSlot> ResourceSlots{};
};

// ── HLSL(D3D12)路径 ──
// HLSL 没有 descriptor set 概念,以 register space 等价之:cbuffer/texture/sampler 的
// `space` == 我们约定的 set 索引。cbuffer 字段从 HlslShaderDesc.Variables 取,
// 资源槽从 BoundResources 按 space 过滤取。
ParsedLayout FromHlsl(
    const render::HlslShaderDesc& desc,
    uint32_t setIndex,
    std::string_view cbufferName) noexcept {
    using namespace radray::render;

    ParsedLayout out{};

    // 1) 在指定 space 上挑一个 cbuffer:优先按名匹配,否则取该 space 上第一个。
    string chosenCBuffer{};
    for (const HlslInputBindDesc& bind : desc.BoundResources) {
        if (bind.Type != HlslShaderInputType::CBUFFER || bind.Space != setIndex) {
            continue;
        }
        if (!cbufferName.empty()) {
            if (bind.Name == cbufferName) {
                chosenCBuffer = bind.Name;
                break;
            }
            continue;
        }
        chosenCBuffer = bind.Name;
        break;
    }

    // 2) 抽取该 cbuffer 的字段表。
    if (!chosenCBuffer.empty()) {
        Nullable<const HlslShaderBufferDesc*> cbOpt = desc.FindCBufferByName(chosenCBuffer);
        if (cbOpt.HasValue()) {
            const HlslShaderBufferDesc* cb = cbOpt.Get();
            out.HasConstantBuffer = true;
            out.CBufferName = chosenCBuffer;
            out.CBufferSize = cb->Size;

            // ConstantBuffer<T> 在 DXC 反射里表现为该 cbuffer 内一个与 cbuffer 同名、
            // 类型为 struct 的唯一变量(IsViewInHlsl)。此时真正的字段是 struct 的成员,
            // 其偏移落在成员类型描述的 Offset 上。需下钻 struct 取字段,
            // 才能与 SPIR-V(直接给出 struct 成员)逐字段对齐。
            bool descended = false;
            if (cb->Variables.size() == 1) {
                size_t varIdx = cb->Variables[0];
                if (varIdx < desc.Variables.size()) {
                    const HlslShaderVariableDesc& var = desc.Variables[varIdx];
                    size_t typeIdx = static_cast<size_t>(var.Type);
                    if (typeIdx < desc.Types.size()) {
                        const HlslShaderTypeDesc& structType = desc.Types[typeIdx];
                        if (structType.Class == HlslShaderVariableClass::STRUCTURE && !structType.Members.empty()) {
                            for (const HlslShaderTypeMember& member : structType.Members) {
                                size_t memberTypeIdx = static_cast<size_t>(member.Type);
                                if (memberTypeIdx >= desc.Types.size()) {
                                    RADRAY_ERR_LOG("MaterialParameterLayout: HLSL member type index {} out of range", memberTypeIdx);
                                    continue;
                                }
                                const HlslShaderTypeDesc& memberType = desc.Types[memberTypeIdx];
                                uint32_t elements = memberType.Elements == 0 ? 1u : memberType.Elements;
                                uint32_t size = static_cast<uint32_t>(memberType.GetSizeInBytes()) * elements;
                                out.Fields.push_back(MaterialParameterLayout::Field{
                                    member.Name,
                                    memberType.Offset,
                                    size});
                            }
                            descended = true;
                        }
                    }
                }
            }

            // 普通 cbuffer(非 ConstantBuffer<T> 视图):变量即字段,偏移取 StartOffset。
            if (!descended) {
                for (size_t varIndex : cb->Variables) {
                    if (varIndex >= desc.Variables.size()) {
                        RADRAY_ERR_LOG("MaterialParameterLayout: HLSL variable index {} out of range", varIndex);
                        continue;
                    }
                    const HlslShaderVariableDesc& var = desc.Variables[varIndex];
                    out.Fields.push_back(MaterialParameterLayout::Field{
                        var.Name,
                        var.StartOffset,
                        var.Size});
                }
            }
        }
    }

    // 3) 资源槽:同一 space 上的 texture / sampler。
    for (const HlslInputBindDesc& bind : desc.BoundResources) {
        if (bind.Space != setIndex) {
            continue;
        }
        if (bind.Type == HlslShaderInputType::TEXTURE) {
            out.ResourceSlots.push_back(MaterialParameterLayout::ResourceSlot{
                bind.Name,
                MaterialParameterLayout::ResourceKind::Texture,
                bind.BindPoint});
        } else if (bind.Type == HlslShaderInputType::SAMPLER) {
            out.ResourceSlots.push_back(MaterialParameterLayout::ResourceSlot{
                bind.Name,
                MaterialParameterLayout::ResourceKind::Sampler,
                bind.BindPoint});
        }
    }

    return out;
}

// ── SPIR-V(Vulkan)路径 ──
// 直接读 descriptor set:UniformBuffer 的字段从 Types[TypeIndex].Members 取,
// 贴图 / 采样器从 ResourceBindings 按 Set 过滤取。
ParsedLayout FromSpirv(
    const render::SpirvShaderDesc& desc,
    uint32_t setIndex,
    std::string_view cbufferName) noexcept {
    using namespace radray::render;

    ParsedLayout out{};

    // 1) 选 set 上的 UniformBuffer。
    const SpirvResourceBinding* chosen = nullptr;
    for (const SpirvResourceBinding& bind : desc.ResourceBindings) {
        if (bind.Kind != SpirvResourceKind::UniformBuffer || bind.Set != setIndex) {
            continue;
        }
        if (!cbufferName.empty()) {
            if (bind.Name == cbufferName) {
                chosen = &bind;
                break;
            }
            continue;
        }
        chosen = &bind;
        break;
    }

    // 2) 抽取字段表(Types[TypeIndex].Members)。
    if (chosen != nullptr) {
        if (chosen->TypeIndex >= desc.Types.size()) {
            RADRAY_ERR_LOG("MaterialParameterLayout: SPIR-V type index {} out of range", chosen->TypeIndex);
        } else {
            const SpirvTypeInfo& type = desc.Types[chosen->TypeIndex];
            out.HasConstantBuffer = true;
            out.CBufferName = chosen->Name;
            out.CBufferSize = chosen->UniformBufferSize != 0 ? chosen->UniformBufferSize : type.Size;
            for (const SpirvTypeMember& member : type.Members) {
                out.Fields.push_back(MaterialParameterLayout::Field{
                    member.Name,
                    member.Offset,
                    member.Size});
            }
        }
    }

    // 3) 资源槽。
    for (const SpirvResourceBinding& bind : desc.ResourceBindings) {
        if (bind.Set != setIndex) {
            continue;
        }
        if (bind.Kind == SpirvResourceKind::SampledImage || bind.Kind == SpirvResourceKind::SeparateImage) {
            out.ResourceSlots.push_back(MaterialParameterLayout::ResourceSlot{
                bind.Name,
                MaterialParameterLayout::ResourceKind::Texture,
                bind.Binding});
        } else if (bind.Kind == SpirvResourceKind::SeparateSampler) {
            out.ResourceSlots.push_back(MaterialParameterLayout::ResourceSlot{
                bind.Name,
                MaterialParameterLayout::ResourceKind::Sampler,
                bind.Binding});
        }
    }

    return out;
}

}  // namespace

std::optional<MaterialParameterLayout> MaterialParameterLayout::CreateFromReflection(
    const render::ShaderReflectionDesc& reflection,
    uint32_t setIndex,
    std::string_view cbufferName) noexcept {
    std::optional<ParsedLayout> parsedOpt = std::visit(
        [&](const auto& desc) -> std::optional<ParsedLayout> {
            using T = std::decay_t<decltype(desc)>;
            if constexpr (std::is_same_v<T, render::HlslShaderDesc>) {
                return FromHlsl(desc, setIndex, cbufferName);
            } else if constexpr (std::is_same_v<T, render::SpirvShaderDesc>) {
                return FromSpirv(desc, setIndex, cbufferName);
            } else {
                return std::nullopt;
            }
        },
        reflection);
    if (!parsedOpt.has_value()) {
        return std::nullopt;
    }
    ParsedLayout& parsed = parsedOpt.value();

    MaterialParameterLayout layout{};
    layout._setIndex = setIndex;
    layout._hasConstantBuffer = parsed.HasConstantBuffer;
    layout._cbufferName = std::move(parsed.CBufferName);
    layout._cbufferSize = parsed.CBufferSize;
    layout._fields = std::move(parsed.Fields);
    layout._resourceSlots = std::move(parsed.ResourceSlots);
    return layout;
}

Nullable<const MaterialParameterLayout::Field*> MaterialParameterLayout::FindField(std::string_view name) const noexcept {
    auto it = std::find_if(_fields.begin(), _fields.end(), [&](const Field& f) {
        return f.Name == name;
    });
    return it == _fields.end() ? Nullable<const Field*>{} : Nullable<const Field*>{&(*it)};
}

string MaterialParameterLayout::GetLayoutSignature() const noexcept {
    // 签名只由反射结果决定:set 索引 + cbuffer(名/大小) + 资源槽(名/种类)。
    // 资源槽按 (名, 种类) 排序后拼接,使顺序无关的反射产生稳定签名。
    // 故意不含原始 binding 号:DXC 对同一 shader 的 DXIL/SPIR-V 会给出不同 binding
    // (DXIL 按 register 类型分名空间,SPIR-V 单一 binding 命名空间),而名/种类跨后端稳定;
    // 同一后端内资源名唯一 → 名/种类 已足以唯一辨识布局(变体增删资源会改变名集,自然使签名不同)。
    string sig;
    sig += "set=";
    sig += std::to_string(_setIndex);
    sig += "|cb=";
    if (_hasConstantBuffer) {
        sig += _cbufferName;
        sig += ':';
        sig += std::to_string(_cbufferSize);
    } else {
        sig += "none";
    }
    vector<const ResourceSlot*> sorted;
    sorted.reserve(_resourceSlots.size());
    for (const ResourceSlot& slot : _resourceSlots) {
        sorted.push_back(&slot);
    }
    std::sort(sorted.begin(), sorted.end(), [](const ResourceSlot* a, const ResourceSlot* b) {
        if (a->Name != b->Name) {
            return a->Name < b->Name;
        }
        return static_cast<uint32_t>(a->Kind) < static_cast<uint32_t>(b->Kind);
    });
    for (const ResourceSlot* slot : sorted) {
        sig += "|r=";
        sig += slot->Name;
        sig += ':';
        sig += (slot->Kind == ResourceKind::Texture) ? 't' : 's';
    }
    return sig;
}

std::optional<StructuredBufferStorage> MaterialParameterLayout::CreateStorageTemplate() const noexcept {
    if (!_hasConstantBuffer) {
        return std::nullopt;
    }
    StructuredBufferStorage::Builder builder{};
    // 单根结构,大小取反射给出的 cbuffer 字节数(已含 16 字节对齐填充)。
    StructuredBufferId cbType = builder.AddType(_cbufferName, _cbufferSize);
    for (const Field& field : _fields) {
        // 每个字段一个原子类型,按反射 offset 落位。
        StructuredBufferId fieldType = builder.AddType(field.Name, field.Size);
        builder.AddMemberForType(cbType, fieldType, field.Name, field.Offset);
    }
    builder.AddRoot(_cbufferName, cbType);
    std::optional<StructuredBufferStorage> storage = builder.Build();
    if (!storage.has_value()) {
        RADRAY_ERR_LOG("MaterialParameterLayout: failed to build storage template for cbuffer '{}'", _cbufferName);
    }
    return storage;
}

}  // namespace radray
