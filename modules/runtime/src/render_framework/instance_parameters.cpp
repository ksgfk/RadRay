#include <radray/runtime/render_framework/instance_parameters.h>
#include <algorithm>
#include <cstring>

namespace radray {
uint32_t SceneParameterStream::RowsPerPage() const noexcept { return std::max(1u, 16384u / Layout.Stride); }
std::span<const byte> SceneParameterStream::GetValues(uint32_t slot) const noexcept {
    if (slot >= Rows.size() || !Rows[slot]) return {};
    const auto count = RowsPerPage();
    return {_pages[slot / count].get() + size_t{slot % count} * Layout.Stride, Layout.Stride};
}
void SceneParameterStream::Set(ShapeId id, std::span<const byte> values) {
    if (id.Index >= Rows.size()) Rows.resize(size_t{id.Index} + 1);
    const auto count = RowsPerPage();
    const auto page = id.Index / count;
    if (page >= _pages.size()) {
        _pages.resize(size_t{page} + 1);
        _live.resize(_pages.size());
    }
    if (!_pages[page]) _pages[page] = make_unique<byte[]>(size_t{count} * Layout.Stride);
    if (!Rows[id.Index]) ++_live[page];
    Rows[id.Index] = Row{id};
    std::memcpy(_pages[page].get() + size_t{id.Index % count} * Layout.Stride, values.data(), Layout.Stride);
}
void SceneParameterStream::Remove(uint32_t slot) {
    if (slot >= Rows.size() || !Rows[slot]) return;
    Rows[slot].reset();
    const auto page = slot / RowsPerPage();
    if (!--_live[page]) _pages[page].reset();
}
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
        result.Fields.push_back({string{*name}, type.Offset, type.Size, type.Kind, type.ScalarKind, type.RowCount, type.ColumnCount, type.ElementCount, type.Stride, type.Flags});
    }
    if (!result.IsValid()) return std::nullopt;
    return result;
}
}  // namespace radray
