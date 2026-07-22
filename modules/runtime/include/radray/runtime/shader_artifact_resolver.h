#pragma once

#include <filesystem>
#include <future>
#include <optional>
#include <span>

#include <radray/shader/shader_binary.h>
#include <radray/types.h>

namespace radray::render {
class Dxc;
}

namespace radray {

class AssetManager;
class ShaderAsset;

using render::ComputeShaderSourceIdentity;
using render::ShaderSourceIdentity;
using render::ShaderSourceIdentityResult;

render::ShaderHash ComputeShaderProgramIdentity(
    const render::ShaderPassDesc& pass,
    uint32_t passIndex,
    std::span<const string> fullDefines,
    render::ShaderHash sourceIdentity) noexcept;

struct ShaderResolvedStageArtifact {
    render::ShaderTarget Target{render::ShaderTarget::DXIL};
    uint32_t PassIndex{0};
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};
    vector<string> Defines;
    string EntryPoint;
    vector<byte> Bytecode;
    render::ShaderHash BinaryHash{};
    render::ShaderReflectionDesc Reflection;
    render::ShaderStageInterfaceDesc Interface;
};

struct ShaderResolvedProgram {
    render::ShaderTarget Target{render::ShaderTarget::DXIL};
    uint32_t PassIndex{0};
    vector<string> Defines;
    vector<ShaderResolvedStageArtifact> Stages;
    render::ShaderInterfaceDesc Interface;
    render::ShaderHash SourceIdentity{};
    // Target-independent identity of source + pass compile options + full variant.
    render::ShaderHash ProgramIdentity{};
};

enum class ShaderArtifactSource : uint8_t {
    Baked,
    DiskCache,
    Jit,
};

struct ShaderArtifactResolutionResult {
    shared_ptr<const ShaderResolvedProgram> Program;
    vector<render::ShaderDiagnostic> Diagnostics;
    ShaderArtifactSource Source{ShaderArtifactSource::Jit};

    bool Succeeded() const noexcept { return Program != nullptr && Diagnostics.empty(); }
};

struct ShaderJitStageCompileRequest {
    std::filesystem::path ShaderRoot;
    render::ShaderPassDesc Pass;
    uint32_t PassIndex{0};
    render::ShaderTarget Target{render::ShaderTarget::DXIL};
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};
    vector<string> FullDefines;
    vector<string> ProjectedDefines;
    render::ShaderHash SourceIdentity{};
};

struct ShaderJitStageCompileResult {
    std::optional<ShaderResolvedStageArtifact> Artifact;
    vector<render::ShaderDiagnostic> Diagnostics;

    bool Succeeded() const noexcept { return Artifact.has_value() && Diagnostics.empty(); }
};

class IShaderJitCompiler {
public:
    virtual ~IShaderJitCompiler() noexcept = default;

    virtual render::ShaderHash GetToolchainHash() const noexcept = 0;
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

    /// assetManager provides slot-generation validation and must outlive this resolver.
    ShaderArtifactResolver(
        AssetManager& assetManager,
        Config config,
        shared_ptr<IShaderJitCompiler> compiler) noexcept;
    ShaderArtifactResolver(const ShaderArtifactResolver&) = delete;
    ShaderArtifactResolver(ShaderArtifactResolver&&) = delete;
    ShaderArtifactResolver& operator=(const ShaderArtifactResolver&) = delete;
    ShaderArtifactResolver& operator=(ShaderArtifactResolver&&) = delete;
    ~ShaderArtifactResolver() noexcept;

    Future ResolveAsync(
        const ShaderAsset& asset,
        render::ShaderTarget target,
        uint32_t passIndex,
        vector<string> fullDefines);

    void ClearMemoryCache() noexcept;
    size_t GetProgramCacheSize() const noexcept;
    size_t GetStageCacheSize() const noexcept;
    size_t GetCapturedSourceIdentityCount() const noexcept;

private:
    shared_ptr<State> _state;
};

#if defined(RADRAY_ENABLE_SHADER_JIT)
shared_ptr<IShaderJitCompiler> CreateDxcShaderJitCompiler(shared_ptr<render::Dxc> dxc) noexcept;
#endif

}  // namespace radray
