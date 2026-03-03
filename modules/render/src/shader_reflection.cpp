#include <radray/render/shader_reflection.h>

namespace radray::render {

vector<ShaderParameter> ShaderReflection::ExtractParameters(const HlslShaderDesc& desc) noexcept {
    vector<ShaderParameter> params{};
    params.reserve(desc.BoundResources.size());
    for (const auto& binding : desc.BoundResources) {
        auto type = binding.MapResourceBindType();
        if (type == ResourceBindType::UNKNOWN) {
            continue;
        }
        ShaderParameter p{};
        p.Name = binding.Name;
        p.Type = type;
        p.Register = binding.BindPoint;
        p.Space = binding.Space;
        p.ArrayLength = binding.BindCount;
        p.TypeSizeInBytes = 0;
        p.Stages = binding.Stages;
        p.IsPushConstant = false;
        p.IsBindless = binding.BindCount == 0;
        if (type == ResourceBindType::CBuffer) {
            auto cbOpt = desc.FindCBufferByName(binding.Name);
            if (!cbOpt.has_value()) {
                RADRAY_ERR_LOG("cannot find cbuffer data: {}", binding.Name);
                continue;
            }
            p.TypeSizeInBytes = cbOpt.value().get().Size;
        }
        params.emplace_back(std::move(p));
    }
    return params;
}

vector<ShaderParameter> ShaderReflection::ExtractParameters(const SpirvShaderDesc& desc) noexcept {
    vector<ShaderParameter> params{};
    params.reserve(desc.ResourceBindings.size() + (desc.PushConstants.empty() ? 0u : 1u));
    if (!desc.PushConstants.empty()) {
        const auto& pc = desc.PushConstants.front();
        ShaderParameter p{};
        p.Name = pc.Name;
        p.Type = ResourceBindType::UNKNOWN;
        p.Register = 0;
        p.Space = 0;
        p.ArrayLength = 0;
        p.TypeSizeInBytes = pc.Size;
        p.Stages = pc.Stages;
        p.IsPushConstant = true;
        p.IsBindless = false;
        params.emplace_back(std::move(p));
        if (desc.PushConstants.size() > 1) {
            RADRAY_ERR_LOG("multiple push constants detected, only the first is used: {}", desc.PushConstants.size());
        }
    }
    for (const auto& b : desc.ResourceBindings) {
        auto type = b.MapResourceBindType();
        if (type == ResourceBindType::UNKNOWN) {
            continue;
        }
        uint32_t count = b.ArraySize == 0 ? 1u : b.ArraySize;
        if (b.IsUnboundedArray) {
            count = 0;
        }
        ShaderParameter p{};
        p.Name = b.Name;
        p.Type = type;
        p.Register = b.Binding;
        p.Space = b.Set;
        p.ArrayLength = count;
        p.TypeSizeInBytes = b.UniformBufferSize;
        p.Stages = b.Stages;
        p.IsPushConstant = false;
        p.IsBindless = b.IsUnboundedArray;
        params.emplace_back(std::move(p));
    }
    return params;
}

vector<ShaderParameter> ShaderReflection::ExtractParameters(const MslShaderReflection& desc) noexcept {
    auto mapResourceType = [](const MslArgument& arg) -> ResourceBindType {
        switch (arg.Type) {
            case MslArgumentType::Buffer:
                if (arg.Access == MslAccess::ReadWrite || arg.Access == MslAccess::WriteOnly) {
                    return ResourceBindType::RWBuffer;
                }
                if (arg.BufferDataType == MslDataType::Struct) {
                    return ResourceBindType::CBuffer;
                }
                return ResourceBindType::Buffer;
            case MslArgumentType::Texture:
                if (arg.TextureType == MslTextureType::TexBuffer) {
                    if (arg.Access == MslAccess::ReadWrite || arg.Access == MslAccess::WriteOnly) {
                        return ResourceBindType::RWTexelBuffer;
                    }
                    return ResourceBindType::TexelBuffer;
                }
                if (arg.Access == MslAccess::ReadWrite || arg.Access == MslAccess::WriteOnly) {
                    return ResourceBindType::RWTexture;
                }
                return ResourceBindType::Texture;
            case MslArgumentType::Sampler:
                return ResourceBindType::Sampler;
            default:
                return ResourceBindType::UNKNOWN;
        }
    };
    auto mapStages = [](MslStage stage) -> ShaderStages {
        switch (stage) {
            case MslStage::Vertex: return ShaderStage::Vertex;
            case MslStage::Fragment: return ShaderStage::Pixel;
            case MslStage::Compute: return ShaderStage::Compute;
        }
        return ShaderStage::UNKNOWN;
    };
    struct MergedArg {
        string Name;
        ResourceBindType Type;
        uint32_t Index;
        ShaderStages Stages;
        uint64_t ArrayLength;
        bool IsPushConstant;
        bool IsUnboundedArray;
        uint32_t DescriptorSet;
        uint64_t BufferDataSize;
    };
    auto makeKey = [](const MslArgument& arg) -> uint64_t {
        uint64_t pcBit = arg.IsPushConstant ? 1ULL : 0ULL;
        return (static_cast<uint64_t>(arg.Type) << 48) |
               (pcBit << 40) |
               (static_cast<uint64_t>(arg.DescriptorSet) << 32) |
               arg.Index;
    };
    unordered_map<uint64_t, size_t> mergeMap{};
    vector<MergedArg> merged{};
    mergeMap.reserve(desc.Arguments.size());
    merged.reserve(desc.Arguments.size());
    for (const auto& arg : desc.Arguments) {
        if (!arg.IsActive || arg.Type == MslArgumentType::ThreadgroupMemory) {
            continue;
        }
        auto type = mapResourceType(arg);
        if (type == ResourceBindType::UNKNOWN) {
            continue;
        }
        uint64_t key = makeKey(arg);
        auto it = mergeMap.find(key);
        if (it != mergeMap.end()) {
            merged[it->second].Stages = merged[it->second].Stages | mapStages(arg.Stage);
            continue;
        }
        mergeMap[key] = merged.size();
        merged.push_back(MergedArg{
            arg.Name,
            type,
            arg.Index,
            mapStages(arg.Stage),
            arg.ArrayLength,
            arg.IsPushConstant,
            arg.IsUnboundedArray,
            arg.DescriptorSet,
            arg.BufferDataSize});
    }

    vector<ShaderParameter> params{};
    params.reserve(merged.size());
    for (const auto& m : merged) {
        if (m.IsPushConstant) {
            ShaderParameter p{};
            p.Name = m.Name;
            p.Type = ResourceBindType::UNKNOWN;
            p.Register = m.Index;
            p.Space = 0;
            p.ArrayLength = 0;
            p.TypeSizeInBytes = static_cast<uint32_t>(m.BufferDataSize);
            p.Stages = m.Stages;
            p.IsPushConstant = true;
            p.IsBindless = false;
            params.emplace_back(std::move(p));
            continue;
        }
        uint32_t setIndex = m.DescriptorSet;
        if (desc.UseArgumentBuffers && (m.Stages.HasFlag(ShaderStage::Vertex) || m.Stages.HasFlag(ShaderStage::Pixel))) {
            setIndex = m.DescriptorSet + MetalMaxVertexInputBindings + 1;
        }
        uint32_t count = m.ArrayLength == 0 ? 1u : static_cast<uint32_t>(m.ArrayLength);
        if (m.IsUnboundedArray) {
            count = 0;
        }
        ShaderParameter p{};
        p.Name = m.Name;
        p.Type = m.Type;
        p.Register = m.Index;
        p.Space = setIndex;
        p.ArrayLength = count;
        p.TypeSizeInBytes = static_cast<uint32_t>(m.BufferDataSize);
        p.Stages = m.Stages;
        p.IsPushConstant = false;
        p.IsBindless = m.IsUnboundedArray;
        params.emplace_back(std::move(p));
    }

    if (desc.UseArgumentBuffers) {
        for (const auto& p : params) {
            bool graphicsStage = p.Stages.HasFlag(ShaderStage::Vertex) || p.Stages.HasFlag(ShaderStage::Pixel);
            if (!graphicsStage) {
                continue;
            }
            if (p.IsPushConstant) {
                if (p.Register != MetalMaxVertexInputBindings) {
                    RADRAY_ERR_LOG(
                        "Invalid Metal push constant slot {} for '{}': expected {} when argument buffers are enabled",
                        p.Register, p.Name, MetalMaxVertexInputBindings);
                    return {};
                }
            } else {
                if (p.Space <= MetalMaxVertexInputBindings) {
                    RADRAY_ERR_LOG(
                        "Invalid Metal descriptor-set slot {} for '{}': must be > {} to avoid vertex-buffer/push-constant overlap",
                        p.Space, p.Name, MetalMaxVertexInputBindings);
                    return {};
                }
            }
        }
    }

    return params;
}

namespace {

struct CBufferBuilderMemberDesc {
    string TypeName;
    string MemberName;
    uint32_t Offset{0};
    size_t SizeInBytes{0};
    uint32_t ArrayCount{0};
    std::optional<uint32_t> ChildStructIndex{};
};

template <class EnumerateMembersFn>
void BuildCBufferStructMembers(
    StructuredBufferStorage::Builder& builder,
    uint32_t rootStructIndex,
    StructuredBufferId rootBdType,
    size_t rootSize,
    EnumerateMembersFn&& enumerateMembers) {
    struct BuildCtx {
        uint32_t StructIndex{0};
        StructuredBufferId BdType{StructuredBufferStorage::InvalidId};
        size_t ParentSize{0};
    };
    stack<BuildCtx> stackCtx{};
    stackCtx.push({rootStructIndex, rootBdType, rootSize});
    while (!stackCtx.empty()) {
        auto ctx = stackCtx.top();
        stackCtx.pop();
        auto members = enumerateMembers(ctx.StructIndex, ctx.ParentSize);
        for (const auto& member : members) {
            auto childBdIdx = builder.AddType(member.TypeName, member.SizeInBytes);
            if (member.ArrayCount > 0) {
                builder.AddMemberForType(ctx.BdType, childBdIdx, member.MemberName, member.Offset, member.ArrayCount);
            } else {
                builder.AddMemberForType(ctx.BdType, childBdIdx, member.MemberName, member.Offset);
            }
            if (member.ChildStructIndex.has_value()) {
                stackCtx.push({member.ChildStructIndex.value(), childBdIdx, member.SizeInBytes});
            }
        }
    }
}

}  // namespace

std::optional<StructuredBufferStorage::Builder> ShaderReflection::CreateCBufferLayout(const HlslShaderDesc& desc) noexcept {
    StructuredBufferStorage::Builder builder{};
    builder.SetAlignment(0);
    auto enumerateMembers = [&](uint32_t parent, size_t parentSize) {
        vector<CBufferBuilderMemberDesc> members{};
        const auto& type = desc.Types[parent];
        members.reserve(type.Members.size());
        for (size_t i = 0; i < type.Members.size(); i++) {
            const auto& member = type.Members[i];
            const auto& memberType = desc.Types[member.Type];
            size_t sizeInBytes = 0;
            if (memberType.IsPrimitive()) {
                sizeInBytes = memberType.GetSizeInBytes();
            } else {
                auto rOffset = i == type.Members.size() - 1 ? parentSize : desc.Types[type.Members[i + 1].Type].Offset;
                sizeInBytes = rOffset - memberType.Offset;
                if (memberType.Elements > 0) {
                    sizeInBytes /= memberType.Elements;
                }
            }
            CBufferBuilderMemberDesc m{};
            m.TypeName = memberType.Name;
            m.MemberName = member.Name;
            m.Offset = static_cast<uint32_t>(memberType.Offset);
            m.SizeInBytes = sizeInBytes;
            m.ArrayCount = memberType.Elements;
            m.ChildStructIndex = static_cast<uint32_t>(member.Type);
            members.emplace_back(std::move(m));
        }
        return members;
    };
    for (const auto& res : desc.BoundResources) {
        if (res.Type != HlslShaderInputType::CBUFFER) {
            continue;
        }
        auto cbOpt = desc.FindCBufferByName(res.Name);
        if (!cbOpt.has_value()) {
            RADRAY_ERR_LOG("cannot find cbuffer: {}", res.Name);
            return std::nullopt;
        }
        const auto& cb = cbOpt.value().get();
        if (cb.IsViewInHlsl) {
            RADRAY_ASSERT(cb.Variables.size() == 1);
            size_t varIdx = cb.Variables[0];
            const auto& var = desc.Variables[varIdx];
            const auto& type = desc.Types[var.Type];
            size_t sizeInBytes = cb.Size;
            StructuredBufferId bdTypeIdx = builder.AddType(type.Name, sizeInBytes);
            if (res.BindCount > 1) {
                builder.AddRoot(var.Name, bdTypeIdx, res.BindCount);
            } else {
                builder.AddRoot(var.Name, bdTypeIdx);
            }
            BuildCBufferStructMembers(builder, static_cast<uint32_t>(var.Type), bdTypeIdx, sizeInBytes, enumerateMembers);
        } else {
            for (size_t i = 0; i < cb.Variables.size(); i++) {
                size_t varIdx = cb.Variables[i];
                const auto& var = desc.Variables[varIdx];
                const auto& type = desc.Types[var.Type];
                size_t sizeInBytes = 0;
                if (i == cb.Variables.size() - 1) {
                    sizeInBytes = cb.Size - var.StartOffset;
                } else {
                    sizeInBytes = desc.Variables[cb.Variables[i + 1]].StartOffset - var.StartOffset;
                }
                if (type.Elements > 0) {
                    sizeInBytes /= type.Elements;
                }
                StructuredBufferId bdTypeIdx = builder.AddType(type.Name, sizeInBytes);
                if (type.Elements == 0) {
                    builder.AddRoot(var.Name, bdTypeIdx);
                } else {
                    builder.AddRoot(var.Name, bdTypeIdx, type.Elements);
                }
                BuildCBufferStructMembers(builder, static_cast<uint32_t>(var.Type), bdTypeIdx, sizeInBytes, enumerateMembers);
            }
        }
    }
    return builder;
}

std::optional<StructuredBufferStorage::Builder> ShaderReflection::CreateCBufferLayout(const SpirvShaderDesc& desc) noexcept {
    StructuredBufferStorage::Builder builder{};
    builder.SetAlignment(0);
    auto enumerateMembers = [&](uint32_t parent, size_t /*parentSize*/) {
        vector<CBufferBuilderMemberDesc> members{};
        const auto& type = desc.Types[parent];
        members.reserve(type.Members.size());
        for (const auto& member : type.Members) {
            const auto& memberType = desc.Types[member.TypeIndex];
            size_t sizeInBytes = member.Size;
            if (memberType.ArraySize > 0) {
                sizeInBytes /= memberType.ArraySize;
            }
            CBufferBuilderMemberDesc m{};
            m.TypeName = memberType.Name;
            m.MemberName = member.Name;
            m.Offset = member.Offset;
            m.SizeInBytes = sizeInBytes;
            m.ArrayCount = memberType.ArraySize;
            m.ChildStructIndex = member.TypeIndex;
            members.emplace_back(std::move(m));
        }
        return members;
    };

    for (const auto& res : desc.PushConstants) {
        RADRAY_ASSERT(res.TypeIndex < desc.Types.size());
        auto type = desc.Types[res.TypeIndex];
        StructuredBufferId bdTypeIdx = builder.AddType(type.Name, res.Size);
        builder.AddRoot(res.Name, bdTypeIdx);
        BuildCBufferStructMembers(builder, res.TypeIndex, bdTypeIdx, res.Size, enumerateMembers);
    }
    for (const auto& res : desc.ResourceBindings) {
        if (res.Kind != SpirvResourceKind::UniformBuffer) {
            continue;
        }
        RADRAY_ASSERT(res.TypeIndex < desc.Types.size());
        auto type = desc.Types[res.TypeIndex];
        if (res.IsViewInHlsl) {
            size_t sizeInBytes = res.UniformBufferSize;
            if (res.ArraySize > 0) {
                sizeInBytes /= res.ArraySize;
            }
            StructuredBufferId bdTypeIdx = builder.AddType(type.Name, sizeInBytes);
            if (res.ArraySize == 0) {
                builder.AddRoot(res.Name, bdTypeIdx);
            } else {
                builder.AddRoot(res.Name, bdTypeIdx, res.ArraySize);
            }
            BuildCBufferStructMembers(builder, res.TypeIndex, bdTypeIdx, sizeInBytes, enumerateMembers);
        } else {
            for (size_t i = 0; i < type.Members.size(); i++) {
                const auto& member = type.Members[i];
                const auto& memberType = desc.Types[member.TypeIndex];
                size_t sizeInBytes;
                if (i == type.Members.size() - 1) {
                    sizeInBytes = res.UniformBufferSize - member.Offset;
                } else {
                    sizeInBytes = type.Members[i + 1].Offset - member.Offset;
                }
                if (memberType.ArraySize > 0) {
                    sizeInBytes /= memberType.ArraySize;
                }
                StructuredBufferId bdTypeIdx = builder.AddType(memberType.Name, sizeInBytes);
                if (memberType.ArraySize == 0) {
                    builder.AddRoot(member.Name, bdTypeIdx);
                } else {
                    builder.AddRoot(member.Name, bdTypeIdx, memberType.ArraySize);
                }
                BuildCBufferStructMembers(builder, member.TypeIndex, bdTypeIdx, sizeInBytes, enumerateMembers);
            }
        }
    }
    return builder;
}

std::optional<StructuredBufferStorage::Builder> ShaderReflection::CreateCBufferLayout(const MslShaderReflection& desc) noexcept {
    StructuredBufferStorage::Builder builder{};
    builder.SetAlignment(0);
    auto enumerateMembers = [&](uint32_t structIdx, size_t parentSize) {
        vector<CBufferBuilderMemberDesc> members{};
        if (structIdx >= desc.StructTypes.size()) {
            return members;
        }
        const auto& st = desc.StructTypes[structIdx];
        members.reserve(st.Members.size());
        for (size_t i = 0; i < st.Members.size(); i++) {
            const auto& member = st.Members[i];
            size_t sizeInBytes = 0;
            if (i + 1 < st.Members.size()) {
                sizeInBytes = st.Members[i + 1].Offset - member.Offset;
            } else {
                sizeInBytes = parentSize - member.Offset;
            }
            CBufferBuilderMemberDesc m{};
            m.TypeName = member.Name;
            m.MemberName = member.Name;
            m.Offset = static_cast<uint32_t>(member.Offset);
            m.SizeInBytes = sizeInBytes;
            if (member.DataType == MslDataType::Array && member.ArrayTypeIndex != UINT32_MAX) {
                const auto& arrType = desc.ArrayTypes[member.ArrayTypeIndex];
                if (arrType.ArrayLength > 0) {
                    m.SizeInBytes = sizeInBytes / arrType.ArrayLength;
                    m.ArrayCount = static_cast<uint32_t>(arrType.ArrayLength);
                }
                if (arrType.ElementType == MslDataType::Struct && arrType.ElementStructTypeIndex != UINT32_MAX) {
                    m.ChildStructIndex = arrType.ElementStructTypeIndex;
                }
            } else if (member.DataType == MslDataType::Struct && member.StructTypeIndex != UINT32_MAX) {
                m.ChildStructIndex = member.StructTypeIndex;
            }
            members.emplace_back(std::move(m));
        }
        return members;
    };
    for (const auto& arg : desc.Arguments) {
        if (!arg.IsActive) continue;
        if (arg.Type != MslArgumentType::Buffer) continue;
        if (arg.BufferDataType != MslDataType::Struct) continue;
        if (arg.BufferStructTypeIndex == UINT32_MAX) continue;
        if (arg.Access == MslAccess::ReadWrite || arg.Access == MslAccess::WriteOnly) continue;
        size_t sizeInBytes = arg.BufferDataSize;
        StructuredBufferId bdTypeIdx = builder.AddType(arg.Name, sizeInBytes);
        if (arg.ArrayLength > 0) {
            builder.AddRoot(arg.Name, bdTypeIdx, arg.ArrayLength);
        } else {
            builder.AddRoot(arg.Name, bdTypeIdx);
        }
        BuildCBufferStructMembers(builder, arg.BufferStructTypeIndex, bdTypeIdx, sizeInBytes, enumerateMembers);
    }
    return builder;
}

}  // namespace radray::render
