#pragma once

#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include <radray/basic_math.h>
#include <radray/hash.h>
#include <radray/nullable.h>
#include <radray/render/backend_shader_artifact.h>
#include <radray/types.h>

namespace radray {

enum class ShaderParameterKind : uint8_t {
    Scalar,
    Vector,
    Matrix,
    Texture,
    Sampler,
    // A cbuffer array whose element type the wire contract cannot express:
    // WireTypeRecord::TypeIndex may only reference a root struct, so an array of
    // scalars/vectors/matrices arrives with no element kind and no element size,
    // only a stride and a count. Element extent is known exactly, the type is not,
    // so such parameters accept raw bytes and reject every typed setter.
    Raw,
};

struct ShaderParameterBufferLayout {
    string Name;
    render::BindingHandle Binding;
    uint32_t Group{0};
    uint32_t BindingNumber{0};
    uint32_t Size{0};
};

struct ShaderParameterInfo {
    ShaderParameterKind Kind{ShaderParameterKind::Scalar};
    render::BindingHandle Binding;
    uint32_t Group{0};
    uint32_t BindingNumber{0};
    uint32_t BufferIndex{std::numeric_limits<uint32_t>::max()};
    uint32_t ByteOffset{0};
    uint32_t Size{0};
    uint32_t Stride{0};
    uint32_t ElementCount{0};
};

struct ShaderParameterRecord {
    string Name;
    ShaderParameterInfo Info;
};

class ShaderParameterLayout {
public:
    static std::optional<ShaderParameterLayout> Create(
        const render::BackendShaderArtifact& artifact) noexcept;
    static std::optional<ShaderParameterLayout> Create(
        const shader::ShaderArtifactView& artifact,
        Nullable<render::PipelineLayout*> pipelineLayout = nullptr) noexcept;

    const ShaderParameterInfo* Find(std::string_view name) const noexcept;
    std::span<const ShaderParameterBufferLayout> Buffers() const noexcept { return _buffers; }
    std::span<const ShaderParameterRecord> Parameters() const noexcept { return _parameters; }
    size_t ParameterCount() const noexcept { return _parameters.size(); }

    // Construction hook used by the artifact type-tree walker. Callers should use Create().
    bool AddParameter(string canonicalPath, std::string_view leafName, ShaderParameterInfo info);

private:
    vector<ShaderParameterBufferLayout> _buffers;
    vector<ShaderParameterRecord> _parameters;
    unordered_map<string, size_t, StringHash, StringEqual> _parameterIndices;
    unordered_map<string, size_t, StringHash, StringEqual> _shortParameterIndices;
};

class ShaderParameterStorage {
public:
    explicit ShaderParameterStorage(
        const ShaderParameterLayout* layout = nullptr,
        std::optional<uint32_t> parameterGroup = std::nullopt);

    void Reset() noexcept;
    const ShaderParameterLayout* GetLayout() const noexcept { return _layout; }
    std::span<const byte> GetBufferData(uint32_t bufferIndex) const noexcept;

    bool SetFloat(std::string_view name, float value, uint32_t element = 0) noexcept;
    bool SetFloat2(std::string_view name, const Eigen::Vector2f& value, uint32_t element = 0) noexcept;
    bool SetFloat3(std::string_view name, const Eigen::Vector3f& value, uint32_t element = 0) noexcept;
    bool SetFloat4(std::string_view name, const Eigen::Vector4f& value, uint32_t element = 0) noexcept;
    bool SetInt(std::string_view name, int32_t value, uint32_t element = 0) noexcept;
    bool SetUInt(std::string_view name, uint32_t value, uint32_t element = 0) noexcept;
    bool SetMatrix4x4(std::string_view name, const Eigen::Matrix4f& value, uint32_t element = 0) noexcept;
    bool SetRaw(std::string_view name, std::span<const byte> value, uint32_t element = 0) noexcept;

private:
    bool SetBytes(
        std::string_view name,
        ShaderParameterKind expectedKind,
        std::span<const byte> value,
        uint32_t element) noexcept;

    const ShaderParameterLayout* _layout{nullptr};
    vector<vector<byte>> _bufferData;
};

}  // namespace radray
