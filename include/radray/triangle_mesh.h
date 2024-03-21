#pragma once

#include <vector>
#include <radray/basic_math.h>

namespace radray {

class TriangleMesh {
public:
    std::vector<uint32> indices;
    std::vector<Eigen::Vector3f> positions;
    std::vector<Eigen::Vector3f> normals;
    std::vector<Eigen::Vector2f> uv0;
    std::vector<Eigen::Vector4f> tangents;
    std::vector<Eigen::Vector4f> color0;

    void InitAsCube(float halfExtend) noexcept;
    void InitAsUVSphere(float radius, uint32 numberSlices) noexcept;
};

}  // namespace radray
