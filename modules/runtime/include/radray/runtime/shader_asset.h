#pragma once

#include <filesystem>
#include <future>
#include <optional>
#include <span>
#include <string_view>

#include <radray/render/common.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/shader/shader_binary.h>

namespace radray::shader {
class Dxc;
}

namespace radray {

using shader::ComputeShaderSourceIdentity;
using shader::ShaderColorTargetDesc;
using shader::ShaderComputePassDesc;
using shader::ShaderGraphicsPassDesc;
using shader::ShaderKeywordGroupDesc;
using shader::ShaderKeywordScope;
using shader::ShaderPassDesc;
using shader::ShaderPassProgramDesc;
using shader::ShaderSourceIdentity;
using shader::ShaderSourceIdentityResult;
using shader::ShaderStencilTestDesc;
using shader::ShaderTagDesc;
using shader::ShaderVariantKey;
using shader::ShaderVariantDesc;
using shader::ShaderVariantDomain;
using shader::ShaderBakeSet;

render::CompareFunction ToRenderCompareFunction(shader::CompareFunction value) noexcept;
render::CullMode ToRenderCullMode(shader::CullMode value) noexcept;
render::StencilOperation ToRenderStencilOperation(shader::StencilOperation value) noexcept;
render::BlendFactor ToRenderBlendFactor(shader::BlendFactor value) noexcept;
render::BlendOperation ToRenderBlendOperation(shader::BlendOperation value) noexcept;
render::ColorWrites ToRenderColorWrites(shader::ColorWrites value) noexcept;
render::BlendComponent ToRenderBlendComponent(const shader::BlendComponent& value) noexcept;
render::BlendState ToRenderBlendState(const shader::BlendState& value) noexcept;
render::StencilFaceState ToRenderStencilFaceState(const shader::StencilFaceState& value) noexcept;
render::StencilState ToRenderStencilState(const shader::StencilState& value) noexcept;

shader::ShaderHash ComputeShaderProgramIdentity(
    const shader::ShaderPassDesc& pass,
    uint32_t passIndex,
    std::span<const string> fullDefines,
    shader::ShaderHash sourceIdentity) noexcept;

class ShaderAsset final : public Asset {
public:
    ShaderAsset() noexcept;
    explicit ShaderAsset(vector<ShaderPassDesc> passes) noexcept;
    explicit ShaderAsset(shader::ShaderBinary binary) noexcept;
    ~ShaderAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    bool IsValid() const noexcept;
    uint64_t GetInstanceId() const noexcept { return _instanceId; }
    const vector<ShaderPassDesc>& GetPasses() const noexcept { return _binary.Asset.Passes; }
    const shader::ShaderBinary& GetBinary() const noexcept { return _binary; }

    Nullable<const shader::ShaderStageArtifact*> FindCompiledStage(
        shader::ShaderTarget target,
        uint32_t passIndex,
        shader::ShaderStage stage,
        const vector<string>& defines) const noexcept;
    Nullable<const shader::ShaderProgramVariantArtifact*> FindProgramVariant(
        shader::ShaderTarget target,
        uint32_t passIndex,
        const vector<string>& defines) const noexcept;

    std::optional<uint32_t> FindPassByName(std::string_view name) const noexcept;
    std::optional<uint32_t> FindPassByTag(std::string_view name, std::string_view value) const noexcept;

private:
    shader::ShaderBinary _binary;
    uint64_t _instanceId{0};
    bool _valid{false};
};

StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    const AssetId& assetId,
    const std::filesystem::path& path);

struct ShaderResolvedStageArtifact {
    shader::ShaderTarget Target{shader::ShaderTarget::DXIL};
    uint32_t PassIndex{0};
    shader::ShaderStage Stage{shader::ShaderStage::UNKNOWN};
    vector<string> Defines;
    string EntryPoint;
    vector<byte> Bytecode;
    shader::ShaderHash BinaryHash{};
    shader::ShaderReflectionDesc Reflection;
    shader::ShaderStageInterfaceDesc Interface;
};

struct ShaderResolvedProgram {
    shader::ShaderTarget Target{shader::ShaderTarget::DXIL};
    uint32_t PassIndex{0};
    vector<string> Defines;
    vector<ShaderResolvedStageArtifact> Stages;
    shader::ShaderInterfaceDesc Interface;
    shader::ShaderHash SourceIdentity{};
    // Target-independent identity of source + pass compile options + full variant.
    shader::ShaderHash ProgramIdentity{};
};

enum class ShaderArtifactSource : uint8_t {
    Baked,
    DiskCache,
    Jit,
};

struct ShaderArtifactResolutionResult {
    shared_ptr<const ShaderResolvedProgram> Program;
    vector<shader::ShaderDiagnostic> Diagnostics;
    ShaderArtifactSource Source{ShaderArtifactSource::Jit};

    bool Succeeded() const noexcept { return Program != nullptr && Diagnostics.empty(); }
};

struct ShaderJitStageCompileRequest {
    std::filesystem::path ShaderRoot;
    shader::ShaderPassDesc Pass;
    uint32_t PassIndex{0};
    shader::ShaderTarget Target{shader::ShaderTarget::DXIL};
    shader::ShaderStage Stage{shader::ShaderStage::UNKNOWN};
    vector<string> FullDefines;
    vector<string> ProjectedDefines;
    shader::ShaderHash SourceIdentity{};
};

struct ShaderJitStageCompileResult {
    std::optional<ShaderResolvedStageArtifact> Artifact;
    vector<shader::ShaderDiagnostic> Diagnostics;

    bool Succeeded() const noexcept { return Artifact.has_value() && Diagnostics.empty(); }
};

class IShaderJitCompiler {
public:
    virtual ~IShaderJitCompiler() noexcept = default;

    virtual shader::ShaderHash GetToolchainHash() const noexcept = 0;
    virtual ShaderJitStageCompileResult CompileStage(
        const ShaderJitStageCompileRequest& request) = 0;
};

class ShaderArtifactResolver {
public:
    struct Config {
        std::filesystem::path ShaderRoot;
        std::filesystem::path DiskCacheDirectory;
        bool EnableDiskCache{true};
    };

    using Future = std::shared_future<ShaderArtifactResolutionResult>;
    struct State;

    ShaderArtifactResolver(
        Config config,
        shared_ptr<IShaderJitCompiler> compiler) noexcept;
    ShaderArtifactResolver(const ShaderArtifactResolver&) = delete;
    ShaderArtifactResolver(ShaderArtifactResolver&&) = delete;
    ShaderArtifactResolver& operator=(const ShaderArtifactResolver&) = delete;
    ShaderArtifactResolver& operator=(ShaderArtifactResolver&&) = delete;
    ~ShaderArtifactResolver() noexcept;

    Future ResolveAsync(
        const ShaderAsset& asset,
        shader::ShaderTarget target,
        uint32_t passIndex,
        vector<string> fullDefines);

    void ClearMemoryCache() noexcept;
    size_t GetProgramCacheSize() const noexcept;
    size_t GetStageCacheSize() const noexcept;

private:
    shared_ptr<State> _state;
};

#if defined(RADRAY_ENABLE_SHADER_JIT)
shared_ptr<IShaderJitCompiler> CreateDxcShaderJitCompiler(shared_ptr<shader::Dxc> dxc) noexcept;
#endif

template <>
struct RuntimeTypeTrait<ShaderAsset> {
    static constexpr RuntimeTypeId value{0x1ed35d36, 0xfc77, 0x456e, 0xa9, 0x10, 0x5c, 0xa4, 0x49, 0x69, 0x57, 0xb3};
    using Bases = std::tuple<Asset>;
};

}  // namespace radray
