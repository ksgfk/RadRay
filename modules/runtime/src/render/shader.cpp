#include <radray/runtime/render/shader.h>

namespace radray::srp {

void Shader::AddPass(ShaderPassSource source) {
    _passes.push_back(std::move(source));
}

const ShaderPassSource* Shader::GetPassSource(std::string_view lightMode) const noexcept {
    for (const auto& pass : _passes) {
        auto lm = pass.Tags.LightMode();
        if (lm.has_value() && *lm == lightMode) {
            return &pass;
        }
    }
    return nullptr;
}

bool Shader::HasPass(std::string_view lightMode) const noexcept {
    return GetPassSource(lightMode) != nullptr;
}

const TagSet* Shader::GetTags(std::string_view lightMode) const noexcept {
    const ShaderPassSource* pass = GetPassSource(lightMode);
    return pass != nullptr ? &pass->Tags : nullptr;
}

bool Shader::ResolveTag(const WantedLightModes& wanted, std::string_view* out) const noexcept {
    // wanted 按优先级从高到低;第一个被某 pass 命中的 LightMode 即选中。
    for (const string& want : wanted) {
        if (HasPass(want)) {
            if (out != nullptr) {
                *out = want;
            }
            return true;
        }
    }
    return false;
}

}  // namespace radray::srp
