#include <radray/runtime/shader_asset.h>

#include <radray/logger.h>
#include <radray/render/rhi.h>
#include <radray/runtime/render_resource_recycler.h>

namespace radray {

// ============================ ShaderContent ============================

ShaderContent::ShaderContent(
    AssetContentKey key,
    IRenderResourceRecycler& recycler,
    ShaderAssetDesc desc,
    unique_ptr<ShaderResolver> resolver,
    vector<unique_ptr<ShaderPassProgram>> passes) noexcept
    : _desc(std::move(desc)),
      _resolver(std::move(resolver)),
      _passes(std::move(passes)) {
    // key 只是创建许可证, recycler 归 AssetContentDeleter 持有, 两者在此都不需要落成员。
    (void)key;
    (void)recycler;
}

ShaderContent::~ShaderContent() noexcept = default;

void ShaderContent::ReleaseRenderResources(IRenderResourceRecycler& recycler) noexcept {
    for (const unique_ptr<ShaderPassProgram>& pass : _passes) {
        pass->ReleaseRenderResources(recycler);
    }
    // program 借用 resolver, 故必须先清 program 再清 resolver。
    _passes.clear();
    _resolver.reset();
    _desc = ShaderAssetDesc{};
}

Nullable<ShaderPassProgram*> ShaderContent::FindPass(std::string_view name) noexcept {
    for (const unique_ptr<ShaderPassProgram>& pass : _passes) {
        if (pass->GetName() == name) {
            return pass.get();
        }
    }
    return nullptr;
}

Nullable<const ShaderPassProgram*> ShaderContent::FindPass(std::string_view name) const noexcept {
    for (const unique_ptr<ShaderPassProgram>& pass : _passes) {
        if (pass->GetName() == name) {
            return pass.get();
        }
    }
    return nullptr;
}

Nullable<ShaderPassProgram*> ShaderContent::GetPass(size_t index) noexcept {
    if (index >= _passes.size()) {
        return nullptr;
    }
    return _passes[index].get();
}

// ============================ ShaderAsset ============================

ShaderAsset::ShaderAsset(
    shared_ptr<ShaderContent> content,
    ShaderResolveContext* context,
    PipelineLayoutCache* layoutCache) noexcept
    : _content(std::move(content)),
      _context(context),
      _layoutCache(layoutCache) {
}

ShaderAsset::~ShaderAsset() noexcept = default;

void ShaderAsset::OnUnload(IRenderResourceRecycler& recycler) {
    // 【只放开本槽位那一份引用, 不直接释放 GPU 资源】: 内容可能仍被 PSO 缓存等持有, 那时
    // 它必须继续存活。真正的释放发生在最后一份引用归零时, 由 AssetContentDeleter 统一交给
    // recycler (见 asset.h)。
    //
    // 故本函数【不使用】recycler 形参 —— 它不再是"资产自己释放 GPU 资源"的时机。
    (void)recycler;
    _content.reset();
}

AssetTypeId ShaderAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<ShaderAsset>;
}

// ============================ 加载 ============================

AssetId MakeShaderAssetId(const std::filesystem::path& manifestPath) {
    return MakeAssetIdFromPath("shader", manifestPath);
}

namespace {

/// options 自身是否可用。【与 manifest 内容无关】, 故 LoadShaderAsset 可以在发起加载
/// 之前先跑一遍 —— 那是 dedup 路径唯一能被校验到的时机。
bool ValidateShaderAssetLoadOptions(
    const ShaderAssetLoadOptions& options,
    ShaderAssetDiagnostic& outDiag) noexcept {
    if (!options.Context.HasValue()) {
        outDiag.Message = "ShaderAssetLoadOptions::Context is null";
        return false;
    }
    if (!options.LayoutCache.HasValue()) {
        outDiag.Message = "ShaderAssetLoadOptions::LayoutCache is null";
        return false;
    }
    // layout 缓存必须已绑定 device —— 没有 device 的缓存 GetOrCreate 永远返回 nullptr,
    // 那会让失败推迟到"CreatePipelineLayout failed"那条模糊的诊断上。
    if (!options.LayoutCache.Get()->GetDevice().HasValue()) {
        outDiag.Message = "ShaderAssetLoadOptions::LayoutCache has no device";
        return false;
    }
    return true;
}

}  // namespace

Nullable<unique_ptr<ShaderAsset>> CreateShaderAsset(
    AssetManager& assetManager,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options,
    ShaderAssetDiagnostic& outDiag) noexcept {
    if (!ValidateShaderAssetLoadOptions(options, outDiag)) {
        return nullptr;
    }
    std::optional<ShaderAssetDesc> desc = LoadShaderAssetDesc(manifestPath, outDiag);
    if (!desc.has_value()) {
        return nullptr;
    }
    if (desc->Passes.empty()) {
        outDiag.Message = "the shader manifest declares no pass";
        return nullptr;
    }

    auto resolver = make_unique<ShaderResolver>(*options.Context.Get(), manifestPath);

    vector<unique_ptr<ShaderPassProgram>> passes;
    passes.reserve(desc->Passes.size());
    for (const ShaderPassDesc& pass : desc->Passes) {
        outDiag.PassName = pass.Name;

        std::optional<ShaderVariantDomain> domain =
            ShaderVariantDomain::Build(desc.value(), pass, outDiag);
        if (!domain.has_value()) {
            return nullptr;
        }

        // Source 展开成最终路径后交给 program —— resolver 只收 pass, 不做继承。
        ShaderPassDesc resolvablePass = MakeResolvablePass(desc.value(), pass);

        // storage 是瞬态的 —— 缓存把内容拷进自己的 key, 之后 storage 即可丢弃。
        const ShaderPipelineLayoutStorage layoutStorage = BuildPipelineLayoutStorage(pass);
        IntrusivePtr<SharedPipelineLayout> layout =
            options.LayoutCache.Get()->GetOrCreate(layoutStorage.Get());
        if (!layout.HasValue()) {
            outDiag.Message = "CreatePipelineLayout failed";
            return nullptr;
        }

        std::optional<ShaderVertexInputStorage> vertexInput;
        if (pass.VertexInput.has_value()) {
            vertexInput = BuildVertexInputStorage(pass.VertexInput.value());
        }

        passes.push_back(
            make_unique<ShaderPassProgram>(
                std::move(resolvablePass),
                std::move(domain.value()),
                std::move(layout),
                std::move(vertexInput),
                resolver.get()));
    }

    outDiag = ShaderAssetDiagnostic{};
    // 内容必须经 AssetManager 创建 —— recycler 由那里注入, 见 AssetContentKey。
    shared_ptr<ShaderContent> content = assetManager.MakeContent<ShaderContent>(
        std::move(desc.value()),
        std::move(resolver),
        std::move(passes));
    return make_unique<ShaderAsset>(
        std::move(content),
        options.Context.Get(),
        options.LayoutCache.Get());
}

namespace {

AssetLoadTask LoadShaderAssetTask(
    AssetManager& assetManager,
    std::filesystem::path manifestPath,
    ShaderAssetLoadOptions options) {
    // 【全程同步, 无挂起点】: 只有短促的文件 IO 与建 layout 的 GPU 调用, 不碰 DXC,
    // 故不会阻塞 AssetManager 的单线程泵。字节码留给 GetOrCreateVariant 惰性解析。
    ShaderAssetDiagnostic diag;
    Nullable<unique_ptr<ShaderAsset>> asset =
        CreateShaderAsset(assetManager, manifestPath, options, diag);
    if (asset == nullptr) {
        co_return AssetLoadResult::Failure(
            fmt::format("failed to load shader asset '{}': {}", manifestPath.string(), diag.ToString()));
    }
    co_return AssetLoadResult::Success(asset.Release());
}

}  // namespace

StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options) {
    return LoadShaderAsset(assetManager, MakeShaderAssetId(manifestPath), manifestPath, options);
}

StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    const AssetId& assetId,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options) {
    // 【必须在 Load 之前】: Load 命中既有 slot 时协程一次都不 resume, 校验若只留在
    // CreateShaderAsset 里, 第二次调用带的空 options 会被静默接受。
    ShaderAssetDiagnostic optionsDiag;
    if (!ValidateShaderAssetLoadOptions(options, optionsDiag)) {
        RADRAY_ERR_LOG(
            "LoadShaderAsset('{}'): {}",
            manifestPath.generic_string(),
            optionsDiag.ToString());
        // 【不发起注定失败的加载】: Faulted slot 会把 id 占住, 之后拿对 options 重试会被
        // dedup 命中那个坏 slot。
        return {};
    }

    // dedup 命中的核对。既有资产是用别人的 context / layout cache 建的, 意味着依赖注入
    // 接错了线 —— 调用方拿到的 layout 来自另一个缓存, 建 PSO 的行为无从预测。
    if (StreamingAssetRef<ShaderAsset> existing = assetManager.Find<ShaderAsset>(assetId);
        existing.IsReady()) {
        const ShaderAsset* asset = existing.Get();
        if (asset->GetResolveContext().Get() != options.Context.Get() ||
            asset->GetLayoutCache().Get() != options.LayoutCache.Get()) {
            RADRAY_ABORT(
                "LoadShaderAsset('{}'): the asset already exists but was built with different "
                "shared facilities (existing context={} cache={}, requested context={} cache={}). "
                "All call sites must pass the one process-wide instance.",
                manifestPath.generic_string(),
                static_cast<const void*>(asset->GetResolveContext().Get()),
                static_cast<const void*>(asset->GetLayoutCache().Get()),
                static_cast<const void*>(options.Context.Get()),
                static_cast<const void*>(options.LayoutCache.Get()));
        }
    }

    return assetManager.Load<ShaderAsset>(
        AssetLoadRequest{
            .Id = assetId,
            .Task = LoadShaderAssetTask(assetManager, manifestPath, options),
            .DebugName = manifestPath.generic_string()});
}

}  // namespace radray
