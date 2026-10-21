#pragma once

#include <filesystem>
#include <radray/types.h>
#include <radray/render/rhi.h>

namespace radray::example {
struct DescriptorReuseOptions {
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    string Scene{"shared"};
    uint32_t Objects{128}, Materials{32}, Views{3}, Flights{3}, SteadyFrames{30};
    std::filesystem::path ShaderRoot, Compiler, Artifacts, Output;
    bool IndependentDefinitions() const noexcept { return Scene == "independent"; }
    bool HasDepth() const noexcept { return Scene == "color-depth"; }
    bool HasSecondColor() const noexcept { return Scene == "compatible-color" || Scene == "visible-order"; }
    uint32_t PassCount() const noexcept { return HasDepth() || HasSecondColor() ? 2 : 1; }
    uint32_t ObjectCount() const noexcept { return std::max(Objects, Materials); }
};
array<float, 4> DescriptorMaterialColor(const DescriptorReuseOptions& options, uint32_t material, uint32_t frame);
array<float, 4> DescriptorInstanceColor(uint32_t slot);
Rect DescriptorObjectRect(const DescriptorReuseOptions& options, uint32_t object);
array<float, 4> DescriptorViewValues(uint32_t view);
uint32_t DescriptorVisibleSlot(const DescriptorReuseOptions& options, uint32_t pass, uint32_t object);
bool RunDescriptorReuse(const DescriptorReuseOptions& options);
}  // namespace radray::example
