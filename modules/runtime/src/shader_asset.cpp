#include <radray/runtime/shader_asset.h>

#include <array>

#include <radray/render/rhi.h>
#include <radray/runtime/render_resource_recycler.h>

namespace radray {

// ============================ ShaderAsset ============================

ShaderAsset::ShaderAsset(
    ShaderAssetDesc desc,
    unique_ptr<ShaderResolver> resolver,
    vector<unique_ptr<ShaderPassProgram>> passes) noexcept
    : _desc(std::move(desc)), _resolver(std::move(resolver)), _passes(std::move(passes)) {
}

ShaderAsset::~ShaderAsset() noexcept = default;

void ShaderAsset::OnUnload(IRenderResourceRecycler& recycler) {
    for (const unique_ptr<ShaderPassProgram>& pass : _passes) {
        pass->ReleaseRenderResources(recycler);
    }
    // program 借用 resolver, 故必须先清 program 再清 resolver。
    _passes.clear();
    _resolver.reset();
    _desc = ShaderAssetDesc{};
}

AssetTypeId ShaderAsset::GetTypeId() const noexcept {
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

namespace {

uint64_t StableHash64(std::string_view text) noexcept {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : text) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace

AssetId MakeShaderAssetId(const std::filesystem::path& manifestPath) {
    const string key = fmt::format("shader:{}", std::filesystem::absolute(manifestPath).generic_string());
    std::array<uint8_t, Guid::Size> bytes{};
    uint64_t h0 = StableHash64(key);
    uint64_t h1 = StableHash64(fmt::format("{}:salt", key));
    for (size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>((h0 >> ((7 - i) * 8)) & 0xffu);
        bytes[i + 8] = static_cast<uint8_t>((h1 >> ((7 - i) * 8)) & 0xffu);
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x40u);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);
    return AssetId{bytes};
}

Nullable<unique_ptr<ShaderAsset>> CreateShaderAsset(
    render::Device& device,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options,
    ShaderAssetDiagnostic& outDiag) noexcept {
    if (!options.Context.HasValue()) {
        outDiag.Message = "ShaderAssetLoadOptions::Context is null";
        return nullptr;
    }
    if (!options.LayoutCache.HasValue()) {
        outDiag.Message = "ShaderAssetLoadOptions::LayoutCache is null";
        return nullptr;
    }
    // 缓存的 layout 会被本资产的 PSO 消费, 两者必须来自同一 device。不同 device 的
    // root signature / VkPipelineLayout 不可互换, 而错配只会在建 PSO 时才暴露。
    if (options.LayoutCache.Get()->GetDevice().Get() != &device) {
        outDiag.Message = "ShaderAssetLoadOptions::LayoutCache belongs to another device";
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
        SharedPipelineLayoutRef layout =
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
    return make_unique<ShaderAsset>(std::move(desc.value()), std::move(resolver), std::move(passes));
}

namespace {

AssetLoadTask LoadShaderAssetTask(
    render::Device* device,
    std::filesystem::path manifestPath,
    ShaderAssetLoadOptions options) {
    // 【全程同步, 无挂起点】: 只有短促的文件 IO 与建 layout 的 GPU 调用, 不碰 DXC,
    // 故不会阻塞 AssetManager 的单线程泵。字节码留给 GetOrCreateVariant 惰性解析。
    ShaderAssetDiagnostic diag;
    Nullable<unique_ptr<ShaderAsset>> asset =
        CreateShaderAsset(*device, manifestPath, options, diag);
    if (asset == nullptr) {
        co_return AssetLoadResult::Failure(
            fmt::format("failed to load shader asset '{}': {}", manifestPath.string(), diag.ToString()));
    }
    co_return AssetLoadResult::Success(asset.Release());
}

}  // namespace

StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    render::Device& device,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options) {
    return LoadShaderAsset(assetManager, MakeShaderAssetId(manifestPath), device, manifestPath, options);
}

StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    const AssetId& assetId,
    render::Device& device,
    const std::filesystem::path& manifestPath,
    const ShaderAssetLoadOptions& options) {
    return assetManager.Load<ShaderAsset>(
        AssetLoadRequest{
            .Id = assetId,
            .Task = LoadShaderAssetTask(&device, manifestPath, options),
            .DebugName = manifestPath.generic_string()});
}

}  // namespace radray
