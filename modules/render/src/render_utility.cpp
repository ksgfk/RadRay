#include <radray/render/render_utility.h>

namespace radray::render {

std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept {
    vector<VertexElement> result;
    result.reserve(semantics.size());
    for (const auto& want : semantics) {
        uint32_t wantSize = GetVertexFormatSizeInBytes(want.Format);
        const VertexBufferEntry* found = nullptr;
        for (const auto& l : layouts) {
            uint32_t preSize = GetVertexDataSizeInBytes(l.Type, l.ComponentCount);
            if (l.Semantic == want.Semantic && l.SemanticIndex == want.SemanticIndex && preSize == wantSize) {
                found = &l;
                break;
            }
        }
        if (!found) {
            return std::nullopt;
        }
        VertexElement& ve = result.emplace_back();
        ve.Offset = found->Offset;
        ve.Semantic = found->Semantic;
        ve.SemanticIndex = found->SemanticIndex;
        ve.Format = want.Format;
        ve.Location = want.Location;
    }
    return result;
}

std::optional<StructuredBufferStorage> CreateCBufferStorage(const MergedHlslShaderDesc& desc) noexcept {
    StructuredBufferStorage::Builder builder{};
    builder.SetAlignment(0);
    auto createType = [&](size_t parent, StructuredBufferId bdType, size_t size) {
        struct TypeCreateCtx {
            size_t par;
            StructuredBufferId bd;
            size_t s;
        };
        stack<TypeCreateCtx> s;
        s.push({parent, bdType, size});
        while (!s.empty()) {
            auto ctx = s.top();
            s.pop();
            const auto& type = desc.Types[ctx.par];
            for (size_t i = 0; i < type.Members.size(); i++) {
                const auto& member = type.Members[i];
                const auto& memberType = desc.Types[member.Type];
                size_t sizeInBytes = 0;
                if (memberType.IsPrimitive()) {
                    sizeInBytes = memberType.GetSizeInBytes();
                } else {
                    auto rOffset = i == type.Members.size() - 1 ? ctx.s : desc.Types[type.Members[i + 1].Type].Offset;
                    sizeInBytes = rOffset - memberType.Offset;
                }
                auto childBdIdx = builder.AddType(memberType.Name, sizeInBytes);
                builder.AddMemberForType(ctx.bd, childBdIdx, member.Name, memberType.Offset);
                s.push({member.Type, childBdIdx, sizeInBytes});
            }
        }
    };
    for (const auto& res : desc.BoundResources) {
        if (res.Type != HlslShaderInputType::CBUFFER) {
            continue;
        }
        auto cbOpt = desc.FindCBufferByName(res.Name);
        if (!cbOpt.has_value()) {
            RADRAY_ERR_LOG("{} {}", "cannot find cbuffer", res.Name);
            return std::nullopt;
        }
        const auto& cb = cbOpt.value().get();
        for (size_t i = 0; i < cb.Variables.size(); i++) {
            size_t varIdx = cb.Variables[i];
            RADRAY_ASSERT(varIdx < desc.Variables.size());
            const auto& var = desc.Variables[varIdx];
            RADRAY_ASSERT(var.Type < desc.Types.size());
            const auto& type = desc.Types[var.Type];
            size_t sizeInBytes = cb.IsViewInHlsl ? cb.Size : var.Size;
            StructuredBufferId bdTypeIdx = builder.AddType(type.Name, sizeInBytes);
            builder.AddRoot(var.Name, bdTypeIdx);
            createType(var.Type, bdTypeIdx, sizeInBytes);
        }
    }
    return builder.Build();
}

std::optional<StructuredBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept {
    StructuredBufferStorage::Builder builder{};
    builder.SetAlignment(0);
    auto createType = [&](size_t parent, StructuredBufferId bdType, size_t size) {
        struct TypeCreateCtx {
            size_t par;
            StructuredBufferId bd;
            size_t s;
        };
        stack<TypeCreateCtx> s;
        s.push({parent, bdType, size});
        while (!s.empty()) {
            auto ctx = s.top();
            s.pop();
            const auto& type = desc.Types[ctx.par];
            for (size_t i = 0; i < type.Members.size(); i++) {
                const auto& member = type.Members[i];
                const auto& memberType = desc.Types[member.TypeIndex];
                size_t sizeInBytes = memberType.Size;
                auto childBdIdx = builder.AddType(memberType.Name, sizeInBytes);
                builder.AddMemberForType(ctx.bd, childBdIdx, member.Name, member.Offset);
                s.push({member.TypeIndex, childBdIdx, sizeInBytes});
            }
        }
    };

    for (const auto& res : desc.PushConstants) {
        RADRAY_ASSERT(res.TypeIndex < desc.Types.size());
        auto type = desc.Types[res.TypeIndex];
        StructuredBufferId bdTypeIdx = builder.AddType(type.Name, type.Size);
        builder.AddRoot(res.Name, bdTypeIdx);
        createType(res.TypeIndex, bdTypeIdx, type.Size);
    }
    for (const auto& res : desc.ResourceBindings) {
        if (res.Kind != SpirvResourceKind::UniformBuffer) {
            continue;
        }
        RADRAY_ASSERT(res.TypeIndex < desc.Types.size());
        auto type = desc.Types[res.TypeIndex];
        if (res.IsViewInHlsl) {
            StructuredBufferId bdTypeIdx = builder.AddType(type.Name, type.Size);
            builder.AddRoot(res.Name, bdTypeIdx);
            createType(res.TypeIndex, bdTypeIdx, type.Size);
        } else {
            for (auto member : type.Members) {
                RADRAY_ASSERT(member.TypeIndex < desc.Types.size());
                auto memberType = desc.Types[member.TypeIndex];
                StructuredBufferId bdTypeIdx = builder.AddType(memberType.Name, memberType.Size);
                builder.AddRoot(member.Name, bdTypeIdx);
                createType(member.TypeIndex, bdTypeIdx, memberType.Size);
            }
        }
    }
    return builder.Build();
}

}  // namespace radray::render
