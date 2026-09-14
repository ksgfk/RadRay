#pragma once

#include <optional>

#include <radray/nullable.h>
#include <radray/render/backend_shader_artifact.h>
#include <radray/runtime/shader_parameters.h>
#include <radray/types.h>

namespace radray {

struct ShaderParameterGroupRecipe {
    struct Buffer {
        uint32_t Index;
        bool Dynamic;
    };
    uint32_t Group{0};
    vector<Buffer> Buffers;
    vector<uint32_t> Textures, Samplers;
    size_t TextureCount{0}, SamplerCount{0};
};

class ShaderProgram {
public:
    // The artifact already carries the layout the recipe produced, so the recipe itself is not an
    // input here: the resolved layout is what everything downstream reads.
    static Nullable<unique_ptr<ShaderProgram>> Create(
        render::Device* device,
        render::BackendShaderArtifact artifact) noexcept;

    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram(ShaderProgram&&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
    ShaderProgram& operator=(ShaderProgram&&) = delete;
    ~ShaderProgram() noexcept;

    /// Borrowed stage and entry name; valid for this program's lifetime. Missing stages return nullopt.
    /// The caller creates and owns pipeline states through the RHI device.
    std::optional<render::ShaderEntry> GetStage(shader::ShaderStage stage) const noexcept;
    const render::BackendShaderArtifact& GetArtifact() const noexcept { return _artifact; }
    render::PipelineLayout* GetPipelineLayout() const noexcept { return _artifact.Layout.get(); }
    render::Device* GetDevice() const noexcept { return _device; }
    // True when the named buffer declaration takes its offset at bind time. Dynamic-ness is a
    // property of one declaration, not of a whole descriptor group.
    bool IsBufferDynamic(std::string_view declarationName) const noexcept;
    const ShaderParameterLayout& GetParameterLayout() const noexcept { return _parameterLayout; }
    /// Immutable program lifetime identity; replacements at the same address receive a new value.
    uint64_t GetGeneration() const noexcept { return _generation; }
    // Render-thread preparation. References remain valid until this program is destroyed.
    const ShaderParameterGroupRecipe& GetOrCreateParameterGroupRecipe(uint32_t group);
    size_t GetParameterGroupRecipeCount() const noexcept { return _parameterGroupRecipes.size(); }

private:
    ShaderProgram(
        render::Device* device,
        render::BackendShaderArtifact artifact,
        ShaderParameterLayout parameterLayout,
        unique_ptr<render::Shader> vertexShader,
        string vertexEntry,
        unique_ptr<render::Shader> pixelShader,
        string pixelEntry,
        unique_ptr<render::Shader> computeShader,
        string computeEntry) noexcept;

    render::Device* _device;
    uint64_t _generation;
    render::BackendShaderArtifact _artifact;
    unique_ptr<render::Shader> _vertexShader;
    string _vertexEntry;
    unique_ptr<render::Shader> _pixelShader;
    string _pixelEntry;
    unique_ptr<render::Shader> _computeShader;
    string _computeEntry;
    ShaderParameterLayout _parameterLayout;
    unordered_map<uint32_t, ShaderParameterGroupRecipe> _parameterGroupRecipes;
};

}  // namespace radray
