#pragma once

#include <span>
#include <string_view>
#include <memory>

#include <radray/types.h>
#include <radray/basic_math.h>

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

namespace vertex_utility {

template <class Scalar>
consteval VertexDataType DeduceVertexDataType() {
    using Bare = std::remove_cvref_t<Scalar>;
    static_assert(std::is_arithmetic_v<Bare>, "Vertex attribute scalar must be arithmetic");
    static_assert(sizeof(Bare) == 4, "Vertex attribute scalar must be 32-bit");

    if constexpr (std::is_floating_point_v<Bare>) {
        return VertexDataType::FLOAT;
    } else if constexpr (std::is_integral_v<Bare> && std::is_signed_v<Bare>) {
        return VertexDataType::SINT;
    } else if constexpr (std::is_integral_v<Bare> && std::is_unsigned_v<Bare>) {
        return VertexDataType::UINT;
    } else {
        static_assert(false, "Unsupported vertex attribute scalar type");
    }
}

}  // namespace vertex_utility

template <class TValueType, class Enable = void>
struct VertexAttributeTraits {
    static_assert(false, "Unsupported vertex attribute container type");
};

template <class TValueType>
struct VertexAttributeTraits<TValueType, std::enable_if_t<std::is_arithmetic_v<TValueType>>> {
    using ScalarType = std::remove_cvref_t<TValueType>;
    static constexpr uint16_t ComponentCount = 1;
    static constexpr VertexDataType Type = vertex_utility::DeduceVertexDataType<TValueType>();
};

template <class Scalar, int Rows, int Cols, int Options, int MaxRows, int MaxCols>
struct VertexAttributeTraits<Eigen::Matrix<Scalar, Rows, Cols, Options, MaxRows, MaxCols>> {
    using MatrixT = Eigen::Matrix<Scalar, Rows, Cols, Options, MaxRows, MaxCols>;
    static constexpr bool kIsVector = (Rows == 1 || Cols == 1);
    static_assert(kIsVector, "Only Eigen vector attributes are supported");
    static_assert(MatrixT::SizeAtCompileTime != Eigen::Dynamic, "Dynamic-sized Eigen vectors are not supported");
    using ScalarType = Scalar;
    static constexpr uint16_t ComponentCount = static_cast<uint16_t>(MatrixT::SizeAtCompileTime);
    static constexpr VertexDataType Type = vertex_utility::DeduceVertexDataType<Scalar>();
};

namespace vertex_utility {

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

}  // namespace vertex_utility

}  // namespace radray
