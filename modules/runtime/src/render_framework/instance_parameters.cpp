#include <radray/runtime/render_framework/instance_parameters.h>

namespace radray {
bool InstanceParameterLayout::IsValid() const noexcept {
    if (Name.empty() || !Stride || Stride > 65536 || Stride % 4 || Fields.empty()) return false;
    for (size_t i = 0; i < Fields.size(); ++i) {
        const auto& f = Fields[i];
        if (f.Name.empty() || !f.Size || f.Offset > Stride || f.Size > Stride - f.Offset) return false;
        for (size_t j = 0; j < i; ++j)
            if (f.Name == Fields[j].Name || (f.Offset < Fields[j].Offset + Fields[j].Size && Fields[j].Offset < f.Offset + f.Size)) return false;
    }
    return true;
}
std::optional<InstanceParameterLayout> ReflectInstanceParameterLayout(const shader::ShaderArtifactView& artifact,
                                                                     std::string_view declaration, string streamName) {
    const auto binding = artifact.FindBinding(declaration);
    if (!binding || binding->Record.Type != uint32_t(shader::ShaderBindingKind::CBuffer) || binding->Record.TypeIndex >= artifact.Types().size()) return std::nullopt;
    const auto root = binding->Record.TypeIndex;
    InstanceParameterLayout result{std::move(streamName), artifact.Types()[root].Size, {}};
    for (const auto& type : artifact.Types()) {
        if (type.ParentIndex != root) continue;
        const auto name = artifact.GetName(type.Name);
        if (!name) return std::nullopt;
        result.Fields.push_back({string{*name}, type.Offset, type.Size, type.Kind, type.ScalarKind, type.RowCount, type.ColumnCount});
    }
    if (!result.IsValid()) return std::nullopt;
    return result;
}
}  // namespace radray
