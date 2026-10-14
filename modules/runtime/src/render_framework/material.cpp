#include <radray/runtime/render_framework/material.h>
#include <radray/runtime/render_system.h>
#include <algorithm>

namespace radray {
MaterialPass::MaterialPass(shared_ptr<const MaterialPass> definition)
    : _definition(definition && definition->_definition ? definition->_definition : std::move(definition)) {
    if (!_definition) RADRAY_ABORT("MaterialPass requires an immutable definition");
}
const MaterialPass& MaterialPass::GetDefinition() const noexcept {
    return _definition ? *_definition : *this;
}
Nullable<ShaderProgram*> MaterialPass::ResolveProgram(RenderSystem& renderer) const {
    const auto& definition = GetDefinition();
    if (definition.RegisteredProgram) return definition.Program.SourceName.empty() ? definition.RegisteredProgram : nullptr;
    return definition.Program.SourceName.empty() ? nullptr : renderer.GetOrCreateShaderProgram(definition.Program);
}
Material::Material(vector<MaterialPass> passes, vector<vector<byte>> constants,
                   vector<StreamingAssetRef<TextureAsset>> textures, vector<render::SamplerDescriptor> samplers)
    : _textures(std::move(textures)), _data{std::move(passes), std::move(constants), {}, std::move(samplers)} {
    for (auto& pass : _data.Passes) pass = MaterialPass{make_shared<const MaterialPass>(std::move(pass))};
    for (const auto& texture : _textures) {
        const auto asset = texture.Get();
        if (!asset || !asset->IsValid()) return;
        _data.Textures.push_back(asset->GetSrv());
    }
    for (size_t i = 0; i < _data.Passes.size(); ++i) {
        const auto& pass = _data.Passes[i].GetDefinition();
        if (pass.Name.empty() || (pass.Program.SourceName.empty() == !pass.RegisteredProgram)) return;
        for (size_t j = 0; j < i; ++j)
            if (pass.Name == _data.Passes[j].GetDefinition().Name) return;
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
                case MaterialInputSource::InstanceParameters:
                    if (input.ValueIndex >= pass.ParameterLayouts.size() || !pass.ParameterLayouts[input.ValueIndex].IsValid()) return;
                    break;
                case MaterialInputSource::SceneObjects:
                case MaterialInputSource::ViewConstants:
                case MaterialInputSource::FrameConstants: break;
                case MaterialInputSource::VisibleInstanceSlots: break;
                default: return;
            }
        }
    }
    _valid = !_data.Passes.empty();
}
Material::~Material() noexcept = default;
void Material::OnUnload(AssetManager&) {}
MaterialBindings::MaterialBindings(vector<MaterialBinding> values)
    : _values(make_shared<const vector<MaterialBinding>>(std::move(values))) {}
MaterialBindings::MaterialBindings(std::initializer_list<MaterialBinding> values)
    : MaterialBindings(vector<MaterialBinding>{values}) {}
}  // namespace radray
