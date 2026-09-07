#pragma once

#include <filesystem>
#include <mutex>
#include <radray/runtime/shader_program_request.h>
#include <radray/runtime/shader_jit.h>
#include <radray/runtime/shader_program.h>

namespace radray {

// Cache access is serialized; returned programs remain owned until GPU-idle cache destruction.
class ShaderProgramCache {
public:
    ShaderProgramCache(render::Device& device, std::filesystem::path sourceRoot, vector<std::filesystem::path> includePaths);
    ~ShaderProgramCache() noexcept;
    Nullable<ShaderProgram*> GetOrCreateShaderProgram(const ShaderProgramRequest& request);
    Nullable<ShaderProgram*> GetOrCreateShaderProgram(std::span<const byte> bytes, const shader::GpuArtifactHash& expectedIdentity,
                                                     const render::ShaderProgramLayoutRecipe& recipe = {});
    size_t GetProgramCount() const noexcept;
    size_t GetArtifactCount() const noexcept;
    bool InvalidateSource(std::string_view sourceName);

private:
    struct ProgramText {
        string Name;
        string Value;

        friend bool operator==(const ProgramText&, const ProgramText&) = default;
    };

    /// compiler artifact 身份: source、结构化 defines、canonical assignments、完整 policy、target 与
    /// toolchain。layout recipe 不在其中, 因为它不改变编译产物。
    struct ArtifactKey {
        string SourceName;
        uint64_t SourceRevision{0};
        vector<ProgramText> Defines{};
        vector<ProgramText> Assignments{};
        shader::CompilePolicy Policy{};
        shader::ShaderTarget Target{shader::ShaderTarget::DXIL};
        shader::Hash128 Toolchain{};

        friend bool operator==(const ArtifactKey&, const ArtifactKey&) = default;
    };

    struct ArtifactKeyHash {
        size_t operator()(const ArtifactKey& value) const noexcept;
    };

    /// 失败按完整 key 记成显式失败, 而不是留一个空 program: 空条目分不清"还没编译"和"编译失败",
    /// 会让一次失败永久污染这个 key。
    struct ArtifactRecord {
        bool Failed{false};
        uint64_t Identity{0};
        ShaderJitArtifact Artifact{};
    };

    /// program/layout 身份: artifact 身份 + 当前 backend 的 canonical resolved layout hash。
    struct ProgramKey {
        uint64_t ArtifactIdentity{0};
        render::ResolvedLayoutHash LayoutHash{};

        friend bool operator==(const ProgramKey&, const ProgramKey&) = default;
    };

    struct ProgramKeyHash {
        size_t operator()(const ProgramKey& value) const noexcept;
    };

    struct ProgramRecord {
        bool Failed{false};
        unique_ptr<ShaderProgram> Program{};
    };

    Nullable<const ArtifactRecord*> GetOrCompileArtifact(
        const ShaderProgramRequest& request,
        ArtifactKey key);

    render::Device& _device;
    std::filesystem::path _sourceRoot;
    mutable std::mutex _mutex;
    unordered_map<string, uint64_t> _sourceRevisions;
    unique_ptr<ShaderJit> _shaderJit;
    unordered_map<ArtifactKey, ArtifactRecord, ArtifactKeyHash> _shaderArtifacts;
    unordered_map<ProgramKey, ProgramRecord, ProgramKeyHash> _shaderPrograms;
    struct PrecompiledProgram {
        vector<byte> Bytes;
        render::ResolvedLayoutHash LayoutHash{};
        unique_ptr<ShaderProgram> Program;
    };
    vector<PrecompiledProgram> _precompiledPrograms;
    uint64_t _nextArtifactIdentity{1};
};

}  // namespace radray
