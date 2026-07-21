#pragma once

#include <filesystem>
#include <future>
#include <optional>
#include <span>

#include <radray/shader/shader_binary.h>
#include <radray/types.h>

namespace radray::shader {
class Dxc;
}

namespace radray {

class AssetManager;
class ShaderAsset;

using shader::ComputeShaderSourceIdentity;
using shader::ShaderSourceIdentity;
using shader::ShaderSourceIdentityResult;

shader::ShaderHash ComputeShaderProgramIdentity(
    const shader::ShaderPassDesc& pass,
    uint32_t passIndex,
    std::span<const string> fullDefines,
    shader::ShaderHash sourceIdentity) noexcept;

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
        shader::ShaderTarget target,
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
shared_ptr<IShaderJitCompiler> CreateDxcShaderJitCompiler(shared_ptr<shader::Dxc> dxc) noexcept;
#endif

}  // namespace radray
