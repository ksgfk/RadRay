#pragma once

#include <radray/basic_math.h>

namespace radray {

class VertexData;

class TriangleMesh {
public:
    vector<uint32_t> Indices;
    vector<Eigen::Vector3f> Positions;
    vector<Eigen::Vector3f> Normals;
    vector<Eigen::Vector2f> UV0;
    vector<Eigen::Vector4f> Tangents;
    vector<Eigen::Vector4f> Color0;

    bool IsValid() const noexcept;
    uint64_t GetVertexByteSize() const noexcept;
    void ToVertexData(VertexData* data) const noexcept;

    void InitAsCube(float halfExtend) noexcept;
    void InitAsUVSphere(float radius, uint32_t numberSlices) noexcept;

    void CalculateTangent() noexcept;
};

}  // namespace radray
