#include <radray/triangle_mesh.h>

#include <numbers>
#include <radray/vertex_data.h>

namespace radray {

bool TriangleMesh::IsValid() const noexcept {
    return indices.size() > 0 &&
           positions.size() > 0 &&
           positions.size() <= std::numeric_limits<uint32>::max() &&
           (normals.size() == 0 || normals.size() == positions.size()) &&
           (uv0.size() == 0 || uv0.size() == positions.size()) &&
           (tangents.size() == 0 || tangents.size() == positions.size()) &&
           (color0.size() == 0 || color0.size() == positions.size());
}

uint64 TriangleMesh::GetVertexByteSize() const noexcept {
    return positions.size() * 12 +
           normals.size() * 12 +
           uv0.size() * 8 +
           tangents.size() * 16 +
           color0.size() * 16;
}

void TriangleMesh::ToVertexData(VertexData* data) const noexcept {
    {
        data->layouts.emplace_back(VertexLayout{VertexSemantic::POSITION, 0, 12, 0});
        uint32 byteOffset = 12;
        if (normals.size() > 0) {
            data->layouts.emplace_back(VertexLayout{VertexSemantic::NORMAL, 0, 12, byteOffset});
            byteOffset += 12;
        }
        if (uv0.size() > 0) {
            data->layouts.emplace_back(VertexLayout{VertexSemantic::TEXCOORD, 0, 8, byteOffset});
            byteOffset += 8;
        }
        if (tangents.size() > 0) {
            data->layouts.emplace_back(VertexLayout{VertexSemantic::TANGENT, 0, 16, byteOffset});
            byteOffset += 16;
        }
        if (color0.size() > 0) {
            data->layouts.emplace_back(VertexLayout{VertexSemantic::COLOR, 0, 16, byteOffset});
            byteOffset += 16;
        }
        uint64 byteSize = byteOffset * positions.size();
        RADRAY_ASSERT(byteSize == GetVertexByteSize(), "byte size not equal");
        data->vertexData = std::make_unique<uint8[]>(byteSize);
        data->vertexSize = byteSize;
        float* target = reinterpret_cast<float*>(data->vertexData.get());
        for (size_t i = 0; i < positions.size(); i++) {
            std::copy(positions[i].begin(), positions[i].end(), target);
            target += 3;
            if (normals.size() > 0) {
                std::copy(normals[i].begin(), normals[i].end(), target);
                target += 3;
            }
            if (uv0.size() > 0) {
                std::copy(uv0[i].begin(), uv0[i].end(), target);
                target += 2;
            }
            if (tangents.size() > 0) {
                std::copy(tangents[i].begin(), tangents[i].end(), target);
                target += 4;
            }
            if (color0.size() > 0) {
                std::copy(color0[i].begin(), color0[i].end(), target);
                target += 4;
            }
        }
    }
    {
        uint64 elemSize = positions.size() <= std::numeric_limits<uint16>::max() ? 2 : 4;
        uint64 byteSize = indices.size() * elemSize;
        data->indexData = std::make_unique<uint8[]>(byteSize);
        data->indexSize = byteSize;
        if (elemSize == sizeof(decltype(indices[0]))) {
            std::memcpy(data->indexData.get(), indices.data(), data->indexSize);
        } else if (elemSize == 2) {
            uint16* target = reinterpret_cast<uint16*>(data->indexData.get());
            for (auto&& i : indices) {
                *target = static_cast<uint16>(i);
                target++;
            }
        } else {
            RADRAY_ABORT("unreachable");
        }
        data->indexType = elemSize == 2 ? VertexIndexType::UInt16 : VertexIndexType::UInt32;
    }
}

void TriangleMesh::InitAsCube(float halfExtend) noexcept {
    positions = std::vector<Eigen::Vector3f>{
        {-1.0f, -1.0f, -1.0f},
        {-1.0f, -1.0f, 1.0f},
        {1.0f, -1.0f, 1.0f},
        {1.0f, -1.0f, -1.0f},
        {-1.0f, 1.0f, -1.0f},
        {-1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, -1.0f},
        {-1.0f, -1.0f, -1.0f},
        {-1.0f, 1.0f, -1.0f},
        {1.0f, 1.0f, -1.0f},
        {1.0f, -1.0f, -1.0f},
        {-1.0f, -1.0f, 1.0f},
        {-1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {1.0f, -1.0f, 1.0f},
        {-1.0f, -1.0f, -1.0f},
        {-1.0f, -1.0f, 1.0f},
        {-1.0f, 1.0f, 1.0f},
        {-1.0f, 1.0f, -1.0f},
        {1.0f, -1.0f, -1.0f},
        {1.0f, -1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, -1.0f}};
    normals = std::vector<Eigen::Vector3f>{
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
        {-1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f}};
    uv0 = std::vector<Eigen::Vector2f>{
        {0.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 1.0f},
        {1.0f, 0.0f},
        {0.0f, 1.0f},
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f},
        {0.0f, 0.0f},
        {0.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 1.0f},
        {1.0f, 0.0f},
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f},
        {1.0f, 0.0f},
        {0.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 1.0f}};
    tangents = std::vector<Eigen::Vector4f>{
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {-1.0f, 0.0f, 0.0f, 1.0f},
        {-1.0f, 0.0f, 0.0f, 1.0f},
        {-1.0f, 0.0f, 0.0f, 1.0f},
        {-1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, -1.0f, 1.0f},
        {0.0f, 0.0f, -1.0f, 1.0f},
        {0.0f, 0.0f, -1.0f, 1.0f},
        {0.0f, 0.0f, -1.0f, 1.0f}};
    indices = std::vector<uint32>{0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11, 12, 15, 14, 12, 14, 13, 16, 17, 18, 16, 18, 19, 20, 23, 22, 20, 22, 21};
    if (halfExtend != 1) {
        for (auto&& i : positions) {
            i *= halfExtend;
        }
    }
}

void TriangleMesh::InitAsUVSphere(float radius, uint32 numberSlices) noexcept {
    uint32 numberParallels = numberSlices / 2;
    uint32 numberVertices = (numberParallels + 1) * (numberSlices + 1);
    uint32 numberIndices = numberParallels * numberSlices * 6;
    float angleStep = (2.0f * std::numbers::pi_v<float>) / numberSlices;
    positions.resize(numberVertices);
    normals.resize(numberVertices);
    uv0.resize(numberVertices);
    tangents.resize(numberVertices);
    for (uint32 i = 0; i < numberParallels + 1; i++) {
        for (uint32 j = 0; j < (numberSlices + 1); j++) {
            uint32 vertexIndex = (i * (numberSlices + 1) + j);
            uint32 normalIndex = (i * (numberSlices + 1) + j);
            uint32 texCoordsIndex = (i * (numberSlices + 1) + j);
            uint32 tangentIndex = (i * (numberSlices + 1) + j);
            float px = radius * std::sin(angleStep * (float)i) * std::sin(angleStep * (float)j);
            float py = radius * std::cos(angleStep * (float)i);
            float pz = radius * std::sin(angleStep * (float)i) * std::cos(angleStep * (float)j);
            positions[vertexIndex] = {px, py, pz};
            float nx = positions[vertexIndex].x() / radius;
            float ny = positions[vertexIndex].y() / radius;
            float nz = positions[vertexIndex].z() / radius;
            normals[normalIndex] = Eigen::Vector3f{nx, ny, nz}.normalized();
            float tx = (float)j / (float)numberSlices;
            float ty = 1.0f - (float)i / (float)numberParallels;
            uv0[texCoordsIndex] = {tx, ty};
            Eigen::AngleAxisf mat(Radian(360.0f * uv0[texCoordsIndex].x()), Eigen::Vector3f{0, 1.0f, 0});
            Eigen::Vector3f tan = mat.matrix() * Eigen::Vector3f{1, 0, 0};
            tangents[tangentIndex] = Eigen::Vector4f{tan.x(), tan.y(), tan.z(), 1.0f};
        }
    }
    uint32 indexIndices = 0;
    indices.resize(numberIndices);
    for (uint32 i = 0; i < numberParallels; i++) {
        for (uint32 j = 0; j < (numberSlices); j++) {
            indices[indexIndices++] = i * (numberSlices + 1) + j;
            indices[indexIndices++] = (i + 1) * (numberSlices + 1) + j;
            indices[indexIndices++] = (i + 1) * (numberSlices + 1) + (j + 1);
            indices[indexIndices++] = i * (numberSlices + 1) + j;
            indices[indexIndices++] = (i + 1) * (numberSlices + 1) + (j + 1);
            indices[indexIndices++] = i * (numberSlices + 1) + (j + 1);
        }
    }
}

}  // namespace radray
