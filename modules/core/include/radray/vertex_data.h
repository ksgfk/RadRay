#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <memory>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

enum class VertexDataType : uint16_t {
    FLOAT,
    UINT,
    SINT
};

class VertexSemantics {
public:
    static constexpr const std::string_view POSITION = "POSITION";
    static constexpr const std::string_view NORMAL = "NORMAL";
    static constexpr const std::string_view TEXCOORD = "TEXCOORD";
    static constexpr const std::string_view TANGENT = "TANGENT";
    static constexpr const std::string_view COLOR = "COLOR";
    static constexpr const std::string_view PSIZE = "PSIZE";
    static constexpr const std::string_view BINORMAL = "BINORMAL";
    static constexpr const std::string_view BLENDINDICES = "BLENDINDICES";
    static constexpr const std::string_view BLENDWEIGHT = "BLENDWEIGHT";
    static constexpr const std::string_view POSITIONT = "POSITIONT";
};

struct VertexBufferEntry {
    string Semantic;
    uint32_t SemanticIndex{0};
    uint32_t BufferIndex{0};
    VertexDataType Type{VertexDataType::FLOAT};
    uint16_t ComponentCount{0};
    uint32_t Offset{0};
    uint32_t Stride{0};
};

struct IndexBufferEntry {
    uint32_t BufferIndex{0};
    uint32_t IndexCount{0};
    uint32_t FormatInBytes{0};
    uint32_t Offset{0};
    uint32_t Stride{0};
};

class MeshBuffer {
public:
    MeshBuffer() = default;
    explicit MeshBuffer(std::span<const byte> data);
    MeshBuffer(const MeshBuffer& other);
    MeshBuffer(MeshBuffer&& other) noexcept = default;
    MeshBuffer& operator=(const MeshBuffer& other);
    MeshBuffer& operator=(MeshBuffer&& other) noexcept = default;
    ~MeshBuffer() = default;

    std::span<const byte> GetData() const noexcept;
    size_t GetSize() const noexcept { return _size; }

private:
    void Assign(std::span<const byte> data);

    std::unique_ptr<byte[]> _data;
    size_t _size{0};
};

class MeshPrimitive {
public:
    vector<VertexBufferEntry> VertexBuffers;
    IndexBufferEntry IndexBuffer{};
    uint32_t VertexCount{0};
};

class MeshResource {
public:
    vector<MeshPrimitive> Primitives;
    vector<MeshBuffer> Bins;
    string Name;
};

constexpr uint32_t GetVertexDataSizeInBytes(VertexDataType type, uint16_t componentCount) noexcept {
    switch (type) {
        case VertexDataType::FLOAT:
            return 4 * componentCount;
        case VertexDataType::UINT:
            return 4 * componentCount;
        case VertexDataType::SINT:
            return 4 * componentCount;
        default:
            return 0;
    }
}

}  // namespace radray
