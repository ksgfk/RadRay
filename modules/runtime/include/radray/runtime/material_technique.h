#pragma once

#include <radray/runtime/material_state.h>
#include <radray/runtime/shader_parameters.h>

namespace radray {

class ShaderProgram;

struct MaterialPassDesc {
    string Name;
    Nullable<ShaderProgram*> Program{nullptr};
    string MaterialBufferAnchor;
    MaterialPipelineState DefaultPipelineState;
};

struct MaterialPassLayout {
    string Name;
    ShaderProgram* Program;
    string MaterialBufferAnchor;
    std::optional<uint32_t> ParameterGroup;
    std::optional<uint32_t> BufferIndex;
    vector<ShaderParameterRecord> Resources;
    MaterialPipelineState DefaultPipelineState;
};

/// Immutable game-thread authoring contract. Must outlive its Materials; programs outlive all flights.
class MaterialTechnique {
public:
    static Nullable<unique_ptr<MaterialTechnique>> Create(vector<MaterialPassDesc> passes, std::string_view primaryPass);

    const MaterialPassLayout& GetPrimaryPass() const noexcept { return _passes[_primaryPass]; }
    std::string_view GetPrimaryPassName() const noexcept { return GetPrimaryPass().Name; }
    uint32_t GetPrimaryPassIndex() const noexcept { return _primaryPass; }
    std::span<const MaterialPassLayout> Passes() const noexcept { return _passes; }
    Nullable<const MaterialPassLayout*> FindPass(std::string_view name) const noexcept;

    MaterialTechnique(const MaterialTechnique&) = delete;
    MaterialTechnique& operator=(const MaterialTechnique&) = delete;

private:
    MaterialTechnique(vector<MaterialPassLayout> passes, uint32_t primaryPass)
        : _passes(std::move(passes)), _primaryPass(primaryPass) {}

    vector<MaterialPassLayout> _passes;
    uint32_t _primaryPass;
};

}  // namespace radray
