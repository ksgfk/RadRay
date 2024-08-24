#pragma once

#include <radray/basic_math.h>

namespace radray {

class VertexData;

class TriangleMesh {
public:
    radray::vector<uint32_t> indices;
    radray::vector<Eigen::Vector3f> positions;
    radray::vector<Eigen::Vector3f> normals;
    radray::vector<Eigen::Vector2f> uv0;
    radray::vector<Eigen::Vector4f> tangents;
    radray::vector<Eigen::Vector4f> color0;

    bool IsValid() const noexcept;
    uint64_t GetVertexByteSize() const noexcept;
    void ToVertexData(VertexData* data) const noexcept;

    void InitAsCube(float halfExtend) noexcept;
    void InitAsUVSphere(float radius, uint32_t numberSlices) noexcept;
};

}  // namespace radray
