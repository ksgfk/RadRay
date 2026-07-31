#include <radray/runtime/shader_asset.h>

#include <radray/logger.h>
#include <radray/render/rhi.h>

namespace radray {

// ============================ ShaderAsset ============================

ShaderAsset::ShaderAsset(
    ShaderAssetDesc desc,
    unique_ptr<ShaderResolver> resolver,
    vector<unique_ptr<ShaderPassProgram>> passes,
    ShaderResolveContext* context,
    PipelineLayoutCache* layoutCache) noexcept
    : _desc(std::move(desc)),
      _resolver(std::move(resolver)),
      _passes(std::move(passes)),
      _context(context),
      _layoutCache(layoutCache) {
}

ShaderAsset::~ShaderAsset() noexcept = default;

void ShaderAsset::OnUnload(AssetManager& manager) {
    // 【本资产的 GPU 对象只有共享的 PipelineLayout, 它不需要延迟销毁】: layout 按
    // SharedPipelineLayout 的引用计数归零即毁, 而仍在录制中的 PSO 各自持有一份引用
    // (见 PipelineStateCache::GraphicsEntry), 故这里放开自己那份不会拉掉正在用的 layout。
    // 理由详见 pipeline_layout_cache.h。
    //
    // 【那为何还要 OnUnload】: 析构顺序。program 借用 resolver 裸指针, 而
    // ShaderPassProgram::ReleaseRenderResources 要在 resolver 还活着时跑完。成员声明顺序
    // 已经保证了这一点, 但显式做一次让"清理发生在哪"不必靠读声明顺序推断。
    (void)manager;
    for (const unique_ptr<ShaderPassProgram>& pass : _passes) {
        pass->ReleaseRenderResources();
    }
}

RuntimeTypeId ShaderAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<ShaderAsset>;
}

Nullable<ShaderPassProgram*> ShaderAsset::FindPass(std::string_view name) noexcept {
    for (const unique_ptr<ShaderPassProgram>& pass : _passes) {
        if (pass->GetName() == name) {
            return pass.get();
        }
    }
    return nullptr;
}

Nullable<const ShaderPassProgram*> ShaderAsset::FindPass(std::string_view name) const noexcept {
    for (const unique_ptr<ShaderPassProgram>& pass : _passes) {
        if (pass->GetName() == name) {
            return pass.get();
        }
    }
    return nullptr;
}

Nullable<ShaderPassProgram*> ShaderAsset::GetPass(size_t index) noexcept {
    if (index >= _passes.size()) {
        return nullptr;
    }
    return _passes[index].get();
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
    // 【无需再查缓存的 device】: PipelineLayoutCache 构造即要求非空 device 且不会失效,
    // 故"缓存非空"已蕴含"能建 layout"。
    return true;
}

}  // namespace

Nullable<unique_ptr<ShaderAsset>> CreateShaderAsset(
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
    return make_unique<ShaderAsset>(
        std::move(desc.value()),
        std::move(resolver),
        std::move(passes),
        options.Context.Get(),
        options.LayoutCache.Get());
}

namespace {

task<AssetLoadResult> LoadShaderAssetTask(
    std::filesystem::path manifestPath,
    ShaderAssetLoadOptions options) {
    // 【全程同步, 无挂起点】: 只有短促的文件 IO 与建 layout 的 GPU 调用, 不碰 DXC,
    // 故不会阻塞 AssetManager 的单线程泵。字节码留给 GetOrCreateVariant 惰性解析。
    ShaderAssetDiagnostic diag;
    Nullable<unique_ptr<ShaderAsset>> asset =
        CreateShaderAsset(manifestPath, options, diag);
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
            .Task = LoadShaderAssetTask(manifestPath, options),
            .DebugName = manifestPath.generic_string()});
}

}  // namespace radray
