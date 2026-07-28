#include <radray/runtime/shader_program.h>

#include <radray/runtime/render_resource_recycler.h>

namespace radray {

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
