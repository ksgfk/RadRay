#pragma once
#include <radray/runtime/shader_program.h>
#include <radray/runtime/texture_asset.h>
#include <radray/runtime/render_framework/instance_parameters.h>
#include <radray/runtime/render_framework/material_template.h>
#include <radray/runtime/render_framework/material_input.h>

namespace radray {
struct MaterialRenderData {
    vector<MaterialTemplateDescriptor> Passes;
    vector<vector<byte>> Constants;
    vector<render::TextureView*> Textures;
    vector<render::SamplerDescriptor> Samplers;
};
/// Immutable after publication. Replacing parameters creates a new asset/version.
class Material final : public Asset {
public:
    Material(vector<MaterialTemplateDescriptor> passes, vector<vector<byte>> constants,
             vector<StreamingAssetRef<TextureAsset>> textures = {}, vector<render::SamplerDescriptor> samplers = {});
    ~Material() noexcept override;
    void OnUnload(AssetManager& manager) override;
    bool IsValid() const noexcept { return _valid; }
    const MaterialRenderData& GetRenderData() const noexcept { return _data; }

private:
    vector<StreamingAssetRef<TextureAsset>> _textures;
    MaterialRenderData _data;
    bool _valid{false};
};
struct MaterialBinding {
    AssetId Id;
    Nullable<const MaterialRenderData*> Data{nullptr};
    friend bool operator==(const MaterialBinding& a, const MaterialBinding& b) noexcept {
        return a.Id == b.Id && a.Data.Get() == b.Data.Get();
    }
};
using MaterialBindings = vector<MaterialBinding>;
template <>
struct RuntimeTypeTrait<Material> {
    static constexpr RuntimeTypeId value{0x9a054ef2, 0x72ca, 0x4a19, 0xb1, 0x84, 0x46, 0x31, 0x29, 0x05, 0x79, 0x39};
};
}  // namespace radray
