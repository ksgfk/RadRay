#pragma once

#include <radray/basic_math.h>

namespace radray {

class VertexData;

class TriangleMesh {
public:
    radray::vector<uint32_t> Indices;
    radray::vector<Eigen::Vector3f> Positions;
    radray::vector<Eigen::Vector3f> Normals;
    radray::vector<Eigen::Vector2f> UV0;
    radray::vector<Eigen::Vector4f> Tangents;
    radray::vector<Eigen::Vector4f> Color0;

    bool IsValid() const noexcept;
    uint64_t GetVertexByteSize() const noexcept;
    void ToVertexData(VertexData* data) const noexcept;

    void InitAsCube(float halfExtend) noexcept;
    void InitAsUVSphere(float radius, uint32_t numberSlices) noexcept;

    void CalculateTangent() noexcept;
};

}  // namespace radray
