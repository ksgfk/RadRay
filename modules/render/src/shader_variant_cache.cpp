#include <filesystem>

#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/file.h>
#include <radray/render/common.h>
#include <radray/render/shader_variant_cache.h>
#include <radray/render/shader/hlsl.h>
#include <radray/render/shader/spirv.h>

#ifdef RADRAY_ENABLE_DXC
#include <radray/render/shader_compiler/dxc.h>
#endif

#ifdef RADRAY_ENABLE_SPIRV_CROSS
#include <radray/render/shader_compiler/spvc.h>
#endif

namespace radray::render {

std::optional<ShaderVariantKey> BuildShaderVariantKey(const ShaderVariantDescriptor& desc) noexcept {
    if (desc.ProgramId.IsEmpty()) {
        RADRAY_ERR_LOG("shader variant cache: ProgramId is empty (assign an identity before caching)");
        return std::nullopt;
    }
    ShaderVariantKey key{};
    key.ProgramId = desc.ProgramId;
    key.PassIndex = desc.PassIndex;
    key.Bitmask = desc.KeywordBitmask;
    return key;
}

namespace {

// 变体来源策略: 封装"如何得到一个 stage 的字节码 Shader"这一唯一差异。
//
// 变体缓存机制 (按 key 去重、reflection -> CreateShader、走 ShaderBindingLayoutCache 得 layout)
// 由统一的 ShaderVariantCacheImpl 承担, 与来源无关。来源只负责产出单个 stage 的 Shader:
//   - DXC JIT:      运行时用 Dxc 编译 HLSL 源 + 反射 (DxcVariantSource)
//   - 预编译磁盘:   从烘焙产物读字节码 + 反序列化反射 (PrecompiledVariantSource)
//
// ProvideStage 返回的 Shader 须已 SetGuid, 由缓存接管所有权; 失败返回 nullptr。
class ShaderVariantSource {
public:
    virtual ~ShaderVariantSource() noexcept = default;

    virtual unique_ptr<Shader> ProvideStage(
        const ShaderVariantStageDesc& stage,
        const ShaderVariantDescriptor& desc) noexcept = 0;
};

// 后端无关、来源无关的统一变体缓存机制。
// miss 时逐 stage 向 ShaderVariantSource 索取 Shader -> 走 ShaderBindingLayoutCache 得 layout。
class ShaderVariantCacheImpl final : public ShaderVariantCache {
public:
    ShaderVariantCacheImpl(Device* device, ShaderBindingLayoutCache* layoutCache, unique_ptr<ShaderVariantSource> source) noexcept
        : _device(device), _layoutCache(layoutCache), _source(std::move(source)) {}
    ~ShaderVariantCacheImpl() noexcept override { Clear(); }

    bool IsValid() const noexcept override {
        return _device != nullptr && _layoutCache != nullptr && _source != nullptr;
    }

    void Destroy() noexcept override {
        Clear();
        _source.reset();
        _device = nullptr;
        _layoutCache = nullptr;
    }

    Nullable<const CompiledShaderVariant*> GetOrCreate(const ShaderVariantDescriptor& desc) noexcept override {
        auto keyOpt = BuildShaderVariantKey(desc);
        if (!keyOpt.has_value()) {
            return nullptr;
        }
        const ShaderVariantKey& key = keyOpt.value();
        if (auto it = _cache.find(key); it != _cache.end()) {
            return &it->second.Variant;
        }
        if (desc.Stages.empty()) {
            RADRAY_ERR_LOG("shader variant cache: descriptor has no stages");
            return nullptr;
        }

        Entry entry{};
        vector<Shader*> shaders;
        shaders.reserve(desc.Stages.size());

        for (const auto& stage : desc.Stages) {
            unique_ptr<Shader> shader = _source->ProvideStage(stage, desc);
            if (shader == nullptr) {
                return nullptr;
            }
            Shader* raw = shader.get();
            switch (stage.Stage) {
                case ShaderStage::Vertex: entry.Variant.VS = raw; break;
                case ShaderStage::Pixel: entry.Variant.PS = raw; break;
                case ShaderStage::Compute: entry.Variant.CS = raw; break;
                default:
                    RADRAY_ERR_LOG("shader variant cache: unsupported stage {}", stage.Stage);
                    return nullptr;
            }
            shaders.push_back(raw);
            entry.Owned.push_back(std::move(shader));
        }

        ShaderBindingLayoutDescriptor layoutDesc{};
        layoutDesc.Shaders = shaders;
        layoutDesc.StaticSamplers = desc.StaticSamplers;
        auto layoutOpt = _layoutCache->GetOrCreate(layoutDesc);
        if (!layoutOpt.HasValue()) {
            RADRAY_ERR_LOG("shader variant cache: GetOrCreate binding layout failed");
            return nullptr;
        }
        entry.Variant.Layout = layoutOpt.Get();

        auto [it, _] = _cache.emplace(key, std::move(entry));
        return &it->second.Variant;
    }

    void Clear() noexcept override {
        for (auto& kv : _cache) {
            for (auto& s : kv.second.Owned) {
                if (s != nullptr) {
                    s->Destroy();
                }
            }
        }
        _cache.clear();
    }

    uint32_t Count() const noexcept override { return static_cast<uint32_t>(_cache.size()); }

private:
    struct Entry {
        CompiledShaderVariant Variant{};
        vector<unique_ptr<Shader>> Owned{};
    };

    Device* _device;
    ShaderBindingLayoutCache* _layoutCache;
    unique_ptr<ShaderVariantSource> _source;
    unordered_map<ShaderVariantKey, Entry, PodHasher<ShaderVariantKey>, PodEqual<ShaderVariantKey>> _cache;
};

// 用给定来源组装统一缓存。source 的所有权转移给缓存。
Nullable<unique_ptr<ShaderVariantCache>> MakeShaderVariantCache(
    Device* device,
    ShaderBindingLayoutCache* layoutCache,
    unique_ptr<ShaderVariantSource> source) noexcept {
    if (device == nullptr || layoutCache == nullptr || source == nullptr) {
        RADRAY_ERR_LOG("MakeShaderVariantCache: device, layoutCache or source is null");
        return nullptr;
    }
    return unique_ptr<ShaderVariantCache>{
        make_unique<ShaderVariantCacheImpl>(device, layoutCache, std::move(source)).release()};
}

// 预编译磁盘来源: 从烘焙产物读字节码 + 反序列化反射, 不依赖 DXC。
//
// 磁盘布局:
//   <bakeRoot>/<LogicalName>/pass<N>_mask<HEX16>/<target>/{vs,ps,cs}.{dxil|spv}
//                                                <target>/{vs,ps,cs}.refl.json
// target: D3D12 -> "dxil", Vulkan -> "spirv"。
class PrecompiledVariantSource final : public ShaderVariantSource {
public:
    PrecompiledVariantSource(Device* device, std::string_view bakeRoot) noexcept
        : _device(device), _bakeRoot(bakeRoot) {
        const RenderBackend backend = _device->GetBackend();
        if (backend == RenderBackend::Vulkan) {
            _target = "spirv";
            _blobExt = ".spv";
            _category = ShaderBlobCategory::SPIRV;
            _isSpirv = true;
        } else {
            _target = "dxil";
            _blobExt = ".dxil";
            _category = ShaderBlobCategory::DXIL;
            _isSpirv = false;
        }
    }

    unique_ptr<Shader> ProvideStage(
        const ShaderVariantStageDesc& stage,
        const ShaderVariantDescriptor& desc) noexcept override {
        if (desc.LogicalName.empty()) {
            RADRAY_ERR_LOG("precompiled shader cache: descriptor has empty LogicalName; cannot locate baked variant");
            return nullptr;
        }
        const char* prefix = StagePrefix(stage.Stage);
        if (prefix == nullptr) {
            RADRAY_ERR_LOG("precompiled shader cache: unsupported stage {}", stage.Stage);
            return nullptr;
        }

        const std::filesystem::path variantDir = MakeVariantDir(desc);
        const std::filesystem::path blobPath = variantDir / (string{prefix} + _blobExt);
        const std::filesystem::path reflPath = variantDir / (string{prefix} + ".refl.json");

        auto blobOpt = ReadBinaryFile(blobPath);
        if (!blobOpt.has_value()) {
            RADRAY_ERR_LOG("precompiled shader cache: cannot read bytecode '{}' (variant not baked for target '{}'?)",
                           blobPath.string(), _target);
            return nullptr;
        }
        auto reflJson = ReadTextFile(reflPath);
        if (!reflJson.has_value()) {
            RADRAY_ERR_LOG("precompiled shader cache: cannot read reflection '{}'", reflPath.string());
            return nullptr;
        }

        ShaderReflectionDesc reflection{};
        if (_isSpirv) {
            auto reflOpt = DeserializeSpirvShaderDesc(reflJson.value());
            if (!reflOpt.has_value()) {
                RADRAY_ERR_LOG("precompiled shader cache: SPIR-V reflection deserialize failed '{}'", reflPath.string());
                return nullptr;
            }
            reflection = std::move(reflOpt.value());
        } else {
            auto reflOpt = DeserializeHlslShaderDesc(reflJson.value());
            if (!reflOpt.has_value()) {
                RADRAY_ERR_LOG("precompiled shader cache: DXIL reflection deserialize failed '{}'", reflPath.string());
                return nullptr;
            }
            reflection = std::move(reflOpt.value());
        }

        const vector<byte>& blob = blobOpt.value();
        ShaderDescriptor shaderDesc{};
        shaderDesc.Source = std::span<const byte>{blob.data(), blob.size()};
        shaderDesc.Category = _category;
        shaderDesc.Stages = stage.Stage;
        shaderDesc.Reflection = std::move(reflection);
        auto shaderOpt = _device->CreateShader(shaderDesc);
        if (!shaderOpt.HasValue()) {
            RADRAY_ERR_LOG("precompiled shader cache: CreateShader failed for '{}'", blobPath.string());
            return nullptr;
        }
        unique_ptr<Shader> shader = shaderOpt.Release();
        shader->SetGuid(Guid::NewGuid());
        return shader;
    }

private:
    // stage -> 文件名前缀 (vs / ps / cs)。
    static const char* StagePrefix(ShaderStage stage) noexcept {
        switch (stage) {
            case ShaderStage::Vertex: return "vs";
            case ShaderStage::Pixel: return "ps";
            case ShaderStage::Compute: return "cs";
            default: return nullptr;
        }
    }

    std::filesystem::path MakeVariantDir(const ShaderVariantDescriptor& desc) const {
        // pass<N>_mask<16位大写HEX>
        char maskBuf[32];
        std::snprintf(maskBuf, sizeof(maskBuf), "pass%u_mask%016llX",
                      desc.PassIndex, static_cast<unsigned long long>(desc.KeywordBitmask));
        std::filesystem::path dir{_bakeRoot};
        dir /= std::filesystem::path{string{desc.LogicalName}};
        dir /= maskBuf;
        dir /= _target;  // <target> 子目录
        return dir;
    }

    Device* _device;
    string _bakeRoot;
    string _target;
    string _blobExt;
    ShaderBlobCategory _category{ShaderBlobCategory::DXIL};
    bool _isSpirv{false};
};

#ifdef RADRAY_ENABLE_DXC

// DXC JIT 来源: 运行时用 Dxc 编译 HLSL 源 (Vulkan 走 SPIR-V) + 反射, 产出单个 stage 的 Shader。
class DxcVariantSource final : public ShaderVariantSource {
public:
    DxcVariantSource(Device* device, Dxc* dxc) noexcept
        : _device(device), _dxc(dxc) {}

    unique_ptr<Shader> ProvideStage(
        const ShaderVariantStageDesc& stage,
        const ShaderVariantDescriptor& desc) noexcept override {
        const bool isSpirv = _device->GetBackend() == RenderBackend::Vulkan;

        DxcCompileParams params{};
        params.Code = stage.Source;
        params.EntryPoint = stage.EntryPoint;
        params.Stage = stage.Stage;
        params.SM = desc.SM;
        params.Defines = desc.Defines;
        params.Includes = desc.Includes;
        params.IsOptimize = desc.IsOptimize;
        params.IsSpirv = isSpirv;
        params.EnableUnbounded = false;
        auto outputOpt = _dxc->Compile(params);
        if (!outputOpt.has_value()) {
            RADRAY_ERR_LOG("shader variant cache: DXC compile failed for entry '{}'", stage.EntryPoint);
            return nullptr;
        }
        auto output = std::move(outputOpt.value());

        ShaderReflectionDesc reflection{};
        ShaderBlobCategory category = ShaderBlobCategory::DXIL;
        if (isSpirv) {
#ifdef RADRAY_ENABLE_SPIRV_CROSS
            auto reflOpt = ReflectSpirv(SpirvBytecodeView{
                .Data = output.Data,
                .EntryPointName = stage.EntryPoint,
                .Stage = stage.Stage,
            });
            if (!reflOpt.has_value()) {
                RADRAY_ERR_LOG("shader variant cache: SPIR-V reflection failed for entry '{}'", stage.EntryPoint);
                return nullptr;
            }
            reflection = std::move(reflOpt.value());
            category = ShaderBlobCategory::SPIRV;
#else
            RADRAY_ERR_LOG("shader variant cache: SPIR-V Cross is not enabled for this build");
            return nullptr;
#endif
        } else {
            auto reflOpt = _dxc->GetShaderDescFromOutput(output.Refl);
            if (!reflOpt.has_value()) {
                RADRAY_ERR_LOG("shader variant cache: DXIL reflection failed for entry '{}'", stage.EntryPoint);
                return nullptr;
            }
            reflection = std::move(reflOpt.value());
            category = ShaderBlobCategory::DXIL;
        }

        ShaderDescriptor shaderDesc{};
        shaderDesc.Source = std::span<const byte>{output.Data.data(), output.Data.size()};
        shaderDesc.Category = category;
        shaderDesc.Stages = stage.Stage;
        shaderDesc.Reflection = std::move(reflection);
        auto shaderOpt = _device->CreateShader(shaderDesc);
        if (!shaderOpt.HasValue()) {
            RADRAY_ERR_LOG("shader variant cache: CreateShader failed for entry '{}'", stage.EntryPoint);
            return nullptr;
        }
        unique_ptr<Shader> shader = shaderOpt.Release();
        shader->SetGuid(Guid::NewGuid());
        return shader;
    }

private:
    Device* _device;
    Dxc* _dxc;
};

#endif

}  // namespace

// 预编译 shader 变体缓存 (不依赖 DXC), 从磁盘烘焙产物加载。见头文件说明。
Nullable<unique_ptr<ShaderVariantCache>> CreatePrecompiledShaderVariantCache(
    Device* device,
    ShaderBindingLayoutCache* layoutCache,
    std::string_view bakeRoot) noexcept {
    if (device == nullptr || layoutCache == nullptr) {
        RADRAY_ERR_LOG("CreatePrecompiledShaderVariantCache: device or layoutCache is null");
        return nullptr;
    }
    if (bakeRoot.empty()) {
        RADRAY_ERR_LOG("CreatePrecompiledShaderVariantCache: bakeRoot is empty");
        return nullptr;
    }
    return MakeShaderVariantCache(
        device, layoutCache,
        make_unique<PrecompiledVariantSource>(device, bakeRoot));
}

#ifdef RADRAY_ENABLE_DXC

// DXC JIT shader 变体缓存, 运行时编译。见头文件说明。
Nullable<unique_ptr<ShaderVariantCache>> CreateShaderVariantCache(
    Device* device,
    Dxc* dxc,
    ShaderBindingLayoutCache* layoutCache) noexcept {
    if (device == nullptr || dxc == nullptr || layoutCache == nullptr) {
        RADRAY_ERR_LOG("CreateShaderVariantCache: device, dxc or layoutCache is null");
        return nullptr;
    }
    return MakeShaderVariantCache(device, layoutCache, make_unique<DxcVariantSource>(device, dxc));
}

#endif

}  // namespace radray::render
