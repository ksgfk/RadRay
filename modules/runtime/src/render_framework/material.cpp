#include <radray/runtime/render_framework/material.h>
#include <algorithm>

namespace radray {
Material::Material(vector<MaterialPass> passes, vector<vector<byte>> constants,
                   vector<StreamingAssetRef<TextureAsset>> textures, vector<render::SamplerDescriptor> samplers)
    : _textures(std::move(textures)), _data{std::move(passes), std::move(constants), {}, std::move(samplers)} {
    for (const auto& texture : _textures) {
        const auto asset = texture.Get();
        if (!asset || !asset->IsValid()) return;
        _data.Textures.push_back(asset->GetSrv());
    }
    for (size_t i = 0; i < _data.Passes.size(); ++i) {
        const auto& pass = _data.Passes[i];
        if (pass.Name.empty() || pass.Program.SourceName.empty()) return;
        for (size_t j = 0; j < i; ++j)
            if (pass.Name == _data.Passes[j].Name) return;
        for (size_t j = 0; j < pass.Inputs.size(); ++j) {
            const auto& input = pass.Inputs[j];
            if (input.Name.empty()) return;
            for (size_t k = 0; k < j; ++k)
                if (input.Name == pass.Inputs[k].Name) return;
            switch (input.Source) {
                case MaterialInputSource::MaterialConstants:
                    if (input.ValueIndex >= _data.Constants.size() || _data.Constants[input.ValueIndex].empty()) return;
                    break;
                case MaterialInputSource::Texture:
                    if (input.ValueIndex >= _data.Textures.size()) return;
                    break;
                case MaterialInputSource::Sampler:
                    if (input.ValueIndex >= _data.Samplers.size()) return;
                    break;
                case MaterialInputSource::SceneObjects:
                case MaterialInputSource::ViewConstants: break;
                default: return;
            }
        }
    }
    _valid = !_data.Passes.empty();
}
Material::~Material() noexcept = default;
void Material::OnUnload(AssetManager&) {}
}  // namespace radray
