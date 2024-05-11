#pragma once

#include <vector>
#include <radray/basic_math.h>

namespace radray {

class VertexData;

class TriangleMesh {
public:
    std::vector<uint32_t> indices;
    std::vector<Eigen::Vector3f> positions;
    std::vector<Eigen::Vector3f> normals;
    std::vector<Eigen::Vector2f> uv0;
    std::vector<Eigen::Vector4f> tangents;
    std::vector<Eigen::Vector4f> color0;

    bool IsValid() const noexcept;
    uint64_t GetVertexByteSize() const noexcept;
    void ToVertexData(VertexData* data) const noexcept;

    void InitAsCube(float halfExtend) noexcept;
    void InitAsUVSphere(float radius, uint32_t numberSlices) noexcept;
};

}  // namespace radray
