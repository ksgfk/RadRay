#include <radray/runtime/shader_program.h>

#include <radray/runtime/render_resource_recycler.h>

namespace radray {

namespace {

/// manifest 声明的 Type + Residency 折叠为 RHI 的 ShaderParameterBindingType。
render::ShaderParameterBindingType ResolveBindingType(const ShaderBindingDesc& binding) noexcept {
    if (binding.Residency != ShaderBindingResidency::RootDescriptor) {
        return binding.Type;
    }
    switch (binding.Type) {
        case render::ShaderParameterBindingType::CBuffer:
            return render::ShaderParameterBindingType::DynamicCBuffer;
        case render::ShaderParameterBindingType::Buffer:
            return render::ShaderParameterBindingType::DynamicBuffer;
        case render::ShaderParameterBindingType::RWBuffer:
            return render::ShaderParameterBindingType::DynamicRWBuffer;
        default:
            return binding.Type;
    }
}

}  // namespace

render::PipelineLayoutDescriptor ShaderPipelineLayoutStorage::Get() const noexcept {
    render::PipelineLayoutDescriptor desc{};
    desc.ParameterSets = _sets;
    desc.PushConstant = _pushConstant;
    return desc;
}

render::VertexInputState ShaderVertexInputStorage::Get() const noexcept {
    render::VertexInputState state{};
    state.Buffers = _buffers;
    state.Attributes = _attributes;
    return state;
}

ShaderPipelineLayoutStorage BuildPipelineLayoutStorage(const ShaderPassDesc& pass) {
    ShaderPipelineLayoutStorage storage;
    storage._entries.reserve(pass.BindingGroups.size());
    storage._sets.reserve(pass.BindingGroups.size());

    for (const ShaderBindingGroupDesc& group : pass.BindingGroups) {
        auto entries = make_unique<vector<render::ShaderParameterSetLayoutEntryDescriptor>>();
        entries->reserve(group.Bindings.size());
        for (const ShaderBindingDesc& binding : group.Bindings) {
            render::ShaderParameterSetLayoutEntryDescriptor entry{};
            entry.Binding = binding.Binding;
            entry.Type = ResolveBindingType(binding);
            entry.Count = binding.Count;
            entry.Stages = binding.Stages;
            entry.ImmutableSampler = binding.ImmutableSampler;
            entries->push_back(entry);
        }
        render::ShaderParameterSetLayoutDescriptor set{};
        set.GroupIndex = group.Group;
        set.Entries = *entries;
        storage._entries.push_back(std::move(entries));
        storage._sets.push_back(set);
    }

    if (pass.PushConstant.has_value()) {
        const ShaderPushConstantDesc& pc = pass.PushConstant.value();
        render::PushConstantDescriptor descriptor{};
        descriptor.Location = pc.Location;
        descriptor.Size = pc.Size;
        descriptor.Stages = pc.Stages;
        storage._pushConstant = descriptor;
    }
    return storage;
}

ShaderVertexInputStorage BuildVertexInputStorage(const ShaderVertexInputDesc& desc) {
    ShaderVertexInputStorage storage;
    storage._buffers.reserve(desc.Buffers.size());
    for (const ShaderVertexBufferDesc& buffer : desc.Buffers) {
        render::VertexBufferLayout layout{};
        layout.Binding = buffer.Binding;
        layout.ArrayStride = buffer.ArrayStride;
        layout.StepMode = buffer.StepMode;
        storage._buffers.push_back(layout);
    }
    storage._semantics.reserve(desc.Attributes.size());
    storage._attributes.reserve(desc.Attributes.size());
    for (size_t i = 0; i < desc.Attributes.size(); ++i) {
        const ShaderVertexAttributeDesc& source = desc.Attributes[i];
        auto semantic = make_unique<string>(source.Semantic);
        render::VertexAttribute attribute{};
        attribute.BufferBinding = source.BufferBinding;
        attribute.Offset = source.Offset;
        attribute.Semantic = *semantic;
        attribute.SemanticIndex = source.SemanticIndex;
        attribute.Format = source.Format;
        attribute.Location = source.Location.value_or(static_cast<uint32_t>(i));
        storage._semantics.push_back(std::move(semantic));
        storage._attributes.push_back(attribute);
    }
    return storage;
}

render::ShaderDescriptor MakeShaderDescriptor(const ShaderBytecode& bytecode) noexcept {
    return render::ShaderDescriptor{
        .Source = bytecode.Data,
        .Category = bytecode.Category,
        .Stages = bytecode.Stage};
}

// ============================ ShaderProgramVariant ============================

Nullable<const ShaderBytecode*> ShaderProgramVariant::FindBytecode(render::ShaderStage stage) const noexcept {
    for (const StageBlob& blob : _stages) {
        if (blob.Stage == stage) {
            return blob.Bytecode;
        }
    }
    return nullptr;
}

std::optional<std::string_view> ShaderProgramVariant::FindEntryPoint(render::ShaderStage stage) const noexcept {
    for (const StageBlob& blob : _stages) {
        if (blob.Stage == stage) {
            return blob.EntryPoint;
        }
    }
    return std::nullopt;
}

render::ShaderStages ShaderProgramVariant::GetStageMask() const noexcept {
    render::ShaderStages mask{render::ShaderStage::UNKNOWN};
    for (const StageBlob& blob : _stages) {
        mask |= blob.Stage;
    }
    return mask;
}

// ============================ ShaderPassProgram ============================

ShaderPassProgram::ShaderPassProgram(
    ShaderPassDesc pass,
    ShaderVariantDomain domain,
    ShaderPipelineLayoutStorage layoutStorage,
    unique_ptr<render::PipelineLayout> pipelineLayout,
    std::optional<ShaderVertexInputStorage> vertexInput,
    ShaderResolver* resolver) noexcept
    : _pass(std::move(pass)),
      _domain(std::move(domain)),
      _layoutStorage(std::move(layoutStorage)),
      _pipelineLayout(std::move(pipelineLayout)),
      _vertexInput(std::move(vertexInput)),
      _resolver(resolver) {
}

ShaderPassProgram::~ShaderPassProgram() noexcept = default;

std::optional<render::VertexInputState> ShaderPassProgram::GetVertexInputState() const noexcept {
    if (!_vertexInput.has_value()) {
        return std::nullopt;
    }
    return _vertexInput->Get();
}

Nullable<const ShaderBytecode*> ShaderPassProgram::GetOrResolveBytecode(
    render::ShaderStage stage,
    render::ShaderBlobCategory category,
    std::span<const string> defines,
    ShaderAssetDiagnostic& outDiag) noexcept {
    if (_resolver == nullptr) {
        outDiag.PassName = _pass.Name;
        outDiag.Stage = stage;
        outDiag.Message = "the shader pass program has no resolver";
        return nullptr;
    }

    // 先算 key 查一级缓存, 避免重复读盘 / 重编。算不出 key (源文件缺失等) 时不能
    // 直接失败 —— Lenient + 无源码部署是合法的发布包形态, 那条路径由 resolver 内部
    // 用 index 自称的身份补齐。
    std::optional<ShaderHash> sourceIdentity;
    {
        ShaderAssetDiagnostic identityDiag;
        sourceIdentity = _resolver->GetSourceIdentity(_pass.Source, identityDiag);
    }
    std::optional<ShaderHash> key;
    if (sourceIdentity.has_value()) {
        key = ComputeShaderArtifactKey(
            _pass,
            stage,
            category,
            defines,
            sourceIdentity.value(),
            _resolver->GetToolchainHash());
        if (key.has_value()) {
            for (const unique_ptr<BytecodeEntry>& entry : _bytecodes) {
                if (entry->Key == key.value()) {
                    return &entry->Bytecode;
                }
            }
        }
    }

    std::optional<ShaderBytecode> resolved =
        _resolver->Resolve(_pass, stage, category, defines, outDiag);
    if (!resolved.has_value()) {
        return nullptr;
    }

    // 以 resolver 报回的 key 为准: 上面预算的 key 在 Lenient 下可能与之不同 (那时
    // resolver 用的是 index 自称的源码身份)。key 为零时不缓存 —— 零无法区分条目。
    const ShaderHash resolvedKey = resolved->Key;
    if (!resolvedKey.IsZero()) {
        for (const unique_ptr<BytecodeEntry>& entry : _bytecodes) {
            if (entry->Key == resolvedKey) {
                return &entry->Bytecode;
            }
        }
    }

    auto entry = make_unique<BytecodeEntry>();
    entry->Key = resolvedKey;
    entry->Bytecode = std::move(resolved.value());
    const ShaderBytecode* result = &entry->Bytecode;
    _bytecodes.push_back(std::move(entry));
    return result;
}

Nullable<const ShaderProgramVariant*> ShaderPassProgram::GetOrCreateVariant(
    const ShaderVariantKey& variant,
    render::ShaderBlobCategory category,
    ShaderAssetDiagnostic& outDiag) noexcept {
    outDiag.PassName = _pass.Name;

    if (!_domain.IsValid(variant)) {
        outDiag.Message = "the shader variant does not belong to this pass domain";
        return nullptr;
    }

    for (const unique_ptr<VariantEntry>& entry : _variants) {
        if (entry->Category == category && entry->Key == variant) {
            return &entry->Variant;
        }
    }

    ShaderProgramVariant built;
    for (const ShaderStageDesc& stageDesc : _pass.Stages) {
        const vector<string> defines = _domain.CollectDefines(variant, stageDesc.Stage);
        Nullable<const ShaderBytecode*> bytecode =
            GetOrResolveBytecode(stageDesc.Stage, category, defines, outDiag);
        if (bytecode == nullptr) {
            // 【失败不写变体缓存】: 半个变体会在下次命中时被当成完整结果。
            return nullptr;
        }
        // EntryPoint 指向 _pass 内的字符串, 与 program 同寿。
        built._stages.push_back(
            ShaderProgramVariant::StageBlob{
                .Stage = stageDesc.Stage,
                .EntryPoint = stageDesc.EntryPoint,
                .Bytecode = bytecode.Get()});
    }

    if (built._stages.empty()) {
        outDiag.Message = "the shader pass declares no stage";
        return nullptr;
    }

    auto entry = make_unique<VariantEntry>();
    entry->Key = variant;
    entry->Category = category;
    entry->Variant = std::move(built);
    const ShaderProgramVariant* result = &entry->Variant;
    _variants.push_back(std::move(entry));
    return result;
}

Nullable<const ShaderProgramVariant*> ShaderPassProgram::GetOrCreateDefaultVariant(
    render::ShaderBlobCategory category,
    ShaderAssetDiagnostic& outDiag) noexcept {
    return GetOrCreateVariant(_domain.DefaultVariant(), category, outDiag);
}

void ShaderPassProgram::ReleaseRenderResources(IRenderResourceRecycler& recycler) noexcept {
    // 变体与字节码先清: 它们只是 CPU 数据, 但清掉能让"资产已卸载"在调用方那里立刻
    // 表现为 miss 而不是拿到陈旧结果。
    _variants.clear();
    _bytecodes.clear();
    if (_pipelineLayout != nullptr) {
        recycler.RecycleRenderResource(std::move(_pipelineLayout));
    }
}

}  // namespace radray
