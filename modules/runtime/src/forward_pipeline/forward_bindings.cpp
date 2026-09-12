#include "forward_bindings.h"

#include <algorithm>

#include <radray/logger.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/renderer_list.h>

namespace radray::forward_detail {

namespace {
constexpr size_t kView = static_cast<size_t>(StaticBindingRole::View);
constexpr size_t kMaterial = static_cast<size_t>(StaticBindingRole::Material);
constexpr size_t kObject = static_cast<size_t>(StaticBindingRole::Object);
constexpr size_t kPass = static_cast<size_t>(StaticBindingRole::Pass);

bool CompileForwardStatic(const StaticPassCompileInput& input, StaticPassCompileResult& result, bool readOnlyDepth) {
    if (input.Bindings)
        result.Bindings = *input.Bindings;
    else if (const auto bindings = ResolveProgramBindings(*input.Pass.Program)) {
        result.Bindings.Buffers = {bindings->ViewBufferIndex, bindings->MaterialBufferIndex, bindings->ObjectBufferIndex, UINT32_MAX};
        result.Bindings.Groups = {bindings->ViewGroup, bindings->MaterialGroup, bindings->ObjectGroup, bindings->PassGroup.value_or(UINT32_MAX)};
        std::copy(bindings->GroupOrder.begin(), bindings->GroupOrder.end(), result.Bindings.GroupOrder.begin());
        result.Bindings.GroupCount = 3;
        result.Bindings.Valid = true;
    }
    result.NormalState = input.Pass.PipelineState;
    result.NormalState.DepthStencil.DepthTestEnable = true;
    result.NormalState.DepthStencil.DepthCompare = render::CompareFunction::LessEqual;
    result.NormalState.DepthStencil.DepthWriteEnable = !readOnlyDepth && RenderQueueRange::Opaque().Contains(input.Queue);
    if (!result.NormalState.DepthStencil.DepthWriteEnable && result.NormalState.DepthStencil.Stencil)
        result.NormalState.DepthStencil.Stencil->WriteMask = 0;
    result.MirroredState = result.NormalState;
    result.MirroredState.Primitive.FaceClockwise = OppositeFrontFace(result.NormalState.Primitive.FaceClockwise);
    return result.Bindings.Valid && input.Pass.ParameterGroup == result.Bindings.Groups[kMaterial];
}

bool CompileForwardLit(const StaticPassCompileInput& input, StaticPassCompileResult& result) { return CompileForwardStatic(input, result, false); }
bool CompileForwardReadOnlyDepth(const StaticPassCompileInput& input, StaticPassCompileResult& result) { return CompileForwardStatic(input, result, true); }
bool CompileDepthOnly(const StaticPassCompileInput& input, StaticPassCompileResult& result) {
    if (input.Bindings)
        result.Bindings = *input.Bindings;
    else if (const auto bindings = ResolveDepthOnlyProgramBindings(*input.Pass.Program)) {
        result.Bindings.Buffers[kView] = bindings->ViewBufferIndex;
        result.Bindings.Buffers[kObject] = bindings->ObjectBufferIndex;
        result.Bindings.Groups[kView] = bindings->ViewGroup;
        result.Bindings.Groups[kObject] = bindings->ObjectGroup;
        result.Bindings.GroupOrder[0] = bindings->ViewGroup < bindings->ObjectGroup ? kView : kObject;
        result.Bindings.GroupOrder[1] = bindings->ViewGroup < bindings->ObjectGroup ? kObject : kView;
        result.Bindings.GroupCount = 2;
        result.Bindings.Valid = true;
    }
    result.NormalState = input.Pass.PipelineState;
    result.NormalState.DepthStencil.DepthTestEnable = true;
    result.NormalState.DepthStencil.DepthWriteEnable = true;
    result.MirroredState = result.NormalState;
    result.MirroredState.Primitive.FaceClockwise = OppositeFrontFace(result.NormalState.Primitive.FaceClockwise);
    return result.Bindings.Valid && !input.Pass.ParameterGroup;
}
}  // namespace

bool RegisterForwardPassPolicies(RenderPrepareContext& context, const Scene& scene) {
    const PassPolicy policies[]{
        {kForwardLitPolicy, 1, "ForwardLit", CompileForwardLit},
        {kForwardLitReadOnlyDepthPolicy, 1, "ForwardLit", CompileForwardReadOnlyDepth},
        {kDepthOnlyPolicy, 1, "DepthOnly", CompileDepthOnly},
        {kDepthNormalsMotionPolicy, 1, "DepthNormalsMotion", CompileForwardLit},
        {kShadowCasterPolicy, 1, "ShadowCaster", CompileForwardLit}};
    bool success = true;
    for (const auto& policy : policies) success &= context.RegisterScenePolicy(scene, policy);
    return success;
}

ForwardProgramBindings ReadForwardStaticBindings(const StaticBindingRecipe& recipe) noexcept {
    ForwardProgramBindings result{recipe.Buffers[kView], recipe.Buffers[kMaterial], recipe.Buffers[kObject],
                                  recipe.Groups[kView], recipe.Groups[kMaterial], recipe.Groups[kObject]};
    if (recipe.Groups[kPass] != UINT32_MAX) result.PassGroup = recipe.Groups[kPass];
    std::copy_n(recipe.GroupOrder.begin(), result.GroupOrder.size(), result.GroupOrder.begin());
    return result;
}

std::optional<ForwardProgramBindings> ResolveProgramBindings(const ShaderProgram& program) {
    const ShaderParameterLayout& layout = program.GetParameterLayout();
    const auto& artifact = program.GetArtifact().Generic();
    if ((layout.Buffers().size() != 3 && layout.Buffers().size() != 4) || !artifact.RootConstants().empty()) return std::nullopt;
    const auto find = [&](std::string_view name) -> std::optional<uint32_t> {
        for (uint32_t index = 0; index < layout.Buffers().size(); ++index) {
            if (layout.Buffers()[index].Name == name && program.IsBufferDynamic(name)) {
                return index;
            }
        }
        return std::nullopt;
    };
    const auto view = find("ForwardView");
    const auto material = find("ForwardMaterial");
    const auto object = find("ForwardObject");
    if (!view || !material || !object) {
        return std::nullopt;
    }
    std::optional<uint32_t> passGroup;
    for (const auto& buffer : layout.Buffers())
        if (buffer.Name == "ForwardPass") passGroup = buffer.Group;
    if (layout.Buffers().size() == 4 && !passGroup) return std::nullopt;
    ForwardProgramBindings bindings{
        *view, *material, *object,
        layout.Buffers()[*view].Group,
        layout.Buffers()[*material].Group,
        layout.Buffers()[*object].Group, passGroup};
    if (bindings.ViewGroup == bindings.MaterialGroup ||
        bindings.ViewGroup == bindings.ObjectGroup ||
        bindings.MaterialGroup == bindings.ObjectGroup) {
        return std::nullopt;
    }
    if (passGroup && (*passGroup == bindings.ViewGroup || *passGroup == bindings.MaterialGroup || *passGroup == bindings.ObjectGroup)) return std::nullopt;
    for (const std::string_view name : {"AlbedoTexture", "LinearSampler"}) {
        const ShaderParameterInfo* resource = layout.Find(name);
        if (resource != nullptr && resource->Group != bindings.MaterialGroup) {
            return std::nullopt;
        }
    }
    for (const auto& binding : artifact.Bindings()) {
        const auto name = artifact.GetName(binding.Name);
        const auto info = name ? program.GetArtifact().FindBindingInfo(*name) : std::nullopt;
        if (passGroup && info && info->Group == *passGroup) continue;
        const auto kind = static_cast<shader::ShaderBindingKind>(binding.Type);
        if (kind == shader::ShaderBindingKind::CBuffer) {
            if (binding.Count != 1) return std::nullopt;
        } else if ((kind != shader::ShaderBindingKind::Texture && kind != shader::ShaderBindingKind::Sampler) ||
                   binding.Group != bindings.MaterialGroup) {
            return std::nullopt;
        }
    }
    const uint32_t groups[]{bindings.ViewGroup, bindings.MaterialGroup, bindings.ObjectGroup};
    std::sort(bindings.GroupOrder.begin(), bindings.GroupOrder.end(), [&](uint8_t a, uint8_t b) { return groups[a] < groups[b]; });
    return bindings;
}

Nullable<const ForwardProgramBindings*> ForwardBindingCache::Resolve(ShaderProgram* program) {
    auto [found, inserted] = _programs.try_emplace(program);
    auto& entry = found->second;
    if (inserted || entry.Generation != program->GetGeneration()) {
        ++_layoutParses;
        entry = {program->GetGeneration(), ResolveProgramBindings(*program)};
        if (!entry.Bindings.has_value()) {
            RADRAY_ERR_LOG("forward pipeline rejected an incompatible shader program");
        }
    }
    return entry.Bindings.has_value() ? &*entry.Bindings : nullptr;
}

std::optional<DepthOnlyProgramBindings> ResolveDepthOnlyProgramBindings(const ShaderProgram& program) {
    const auto& artifact = program.GetArtifact().Generic();
    if (!artifact.RootConstants().empty()) return std::nullopt;
    for (const auto& binding : artifact.Bindings()) {
        if (static_cast<shader::ShaderBindingKind>(binding.Type) != shader::ShaderBindingKind::CBuffer || binding.Count != 1) return std::nullopt;
    }
    const auto buffers = program.GetParameterLayout().Buffers();
    std::optional<uint32_t> view, object;
    for (uint32_t index = 0; index < buffers.size(); ++index) {
        if (!program.IsBufferDynamic(buffers[index].Name)) return std::nullopt;
        if (buffers[index].Name == "ForwardView")
            view = index;
        else if (buffers[index].Name == "ForwardObject")
            object = index;
        else
            return std::nullopt;
    }
    if (!view || !object || buffers[*view].Group == buffers[*object].Group) return std::nullopt;
    for (const auto& parameter : program.GetParameterLayout().Parameters()) {
        if (parameter.Info.Kind == ShaderParameterKind::Texture || parameter.Info.Kind == ShaderParameterKind::Sampler) return std::nullopt;
    }
    return DepthOnlyProgramBindings{*view, *object, buffers[*view].Group, buffers[*object].Group};
}

Nullable<const DepthOnlyProgramBindings*> DepthOnlyBindingCache::Resolve(ShaderProgram* program) {
    auto [found, inserted] = _programs.try_emplace(program);
    auto& entry = found->second;
    if (inserted || entry.Generation != program->GetGeneration()) {
        ++_layoutParses;
        entry = {program->GetGeneration(), ResolveDepthOnlyProgramBindings(*program)};
        if (!entry.Bindings) RADRAY_ERR_LOG("DepthOnly rejected an incompatible shader program; its depth draws are disabled");
    }
    return entry.Bindings ? &*entry.Bindings : nullptr;
}

}  // namespace radray::forward_detail
