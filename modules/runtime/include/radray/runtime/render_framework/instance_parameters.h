#pragma once

#include <radray/shader/shader_artifact.h>
#include <radray/runtime/render_framework/scene_id.h>
#include <radray/render/rhi.h>

namespace radray {
/// Owned CPU schema. Authors share the declaration used for reflection and structured storage.
struct InstanceParameterLayout {
    struct Field {
        string Name;
        uint32_t Offset, Size, Kind, ScalarKind, Rows, Columns;
        uint32_t ElementCount{0}, Stride{0}, Flags{0};
        friend bool operator==(const Field&, const Field&) = default;
    };
    string Name;
    uint32_t Stride{0};
    vector<Field> Fields;
    bool IsValid() const noexcept;
    friend bool operator==(const InstanceParameterLayout&, const InstanceParameterLayout&) = default;
};
/// Reflects a compiler-published constant payload. Structured declarations must share this
/// exact padded element layout; bindings without element reflection require this explicit schema.
std::optional<InstanceParameterLayout> ReflectInstanceParameterLayout(const shader::ShaderArtifactView& artifact,
                                                                      std::string_view declaration, string streamName);
struct ShapeParameterUpdate {
    ShapeId Id;
    InstanceParameterLayout Layout;
    vector<byte> Values;
};
struct SceneParameterChange {
    ShapeId Id;
    string Stream;
};
struct InstanceParameterBuffer {
    const InstanceParameterLayout* Layout;
    render::ShaderBufferBinding Values;
};
struct SceneParameterStream {
    struct Row {
        ShapeId Id;
        vector<byte> Values;
    };
    InstanceParameterLayout Layout;
    vector<std::optional<Row>> Rows;
};
}  // namespace radray
