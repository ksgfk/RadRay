#include <radray/triangle_mesh.h>

#include <new>
#include <numbers>
#include <limits>
#include <radray/vertex_data.h>

namespace radray {

bool TriangleMesh::IsValid() const noexcept {
    return indices.size() > 0 &&
           positions.size() > 0 &&
           positions.size() <= std::numeric_limits<uint32_t>::max() &&
           (normals.size() == 0 || normals.size() == positions.size()) &&
           (uv0.size() == 0 || uv0.size() == positions.size()) &&
           (tangents.size() == 0 || tangents.size() == positions.size()) &&
           (color0.size() == 0 || color0.size() == positions.size());
}

uint64_t TriangleMesh::GetVertexByteSize() const noexcept {
    return positions.size() * 12 +
           normals.size() * 12 +
           uv0.size() * 8 +
           tangents.size() * 16 +
           color0.size() * 16;
}

void TriangleMesh::ToVertexData(VertexData* data) const noexcept {
    {
        data->layouts.emplace_back(VertexLayout{VertexSemantic::POSITION, 0, 12, 0});
        uint32_t byteOffset = 12;
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
        uint64_t byteSize = byteOffset * positions.size();
        RADRAY_ASSERT(byteSize == GetVertexByteSize());
        data->vertexData = radray::make_unique<byte[]>(byteSize);
        data->vertexSize = byteSize;
        float* target = std::launder(reinterpret_cast<float*>(data->vertexData.get()));
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
        RADRAY_ASSERT(indices.size() <= std::numeric_limits<uint32_t>::max());
        if (positions.size() <= std::numeric_limits<uint16_t>::max()) {
            data->indexType = VertexIndexType::UInt16;
            data->indexSize = indices.size() * sizeof(uint16_t);
            data->indexData = radray::make_unique<byte[]>(data->indexSize);
            data->indexCount = (uint32_t)indices.size();
            std::memcpy(data->indexData.get(), indices.data(), data->indexSize);
        } else if (positions.size() <= std::numeric_limits<uint32_t>::max()) {
            data->indexType = VertexIndexType::UInt32;
            data->indexSize = indices.size() * sizeof(uint32_t);
            data->indexData = radray::make_unique<byte[]>(data->indexSize);
            data->indexCount = (uint32_t)indices.size();
            uint16_t* target = std::launder(reinterpret_cast<uint16_t*>(data->indexData.get()));
            for (auto&& i : indices) {
                *target = static_cast<uint16_t>(i);
                target++;
            }
        } else {
            RADRAY_ABORT("too large mesh {}", positions.size());
        }
    }
}

void TriangleMesh::InitAsCube(float halfExtend) noexcept {
    positions = radray::vector<Eigen::Vector3f>{
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
    normals = radray::vector<Eigen::Vector3f>{
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
    uv0 = radray::vector<Eigen::Vector2f>{
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
    tangents = radray::vector<Eigen::Vector4f>{
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
    indices = radray::vector<uint32_t>{0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11, 12, 15, 14, 12, 14, 13, 16, 17, 18, 16, 18, 19, 20, 23, 22, 20, 22, 21};
    if (halfExtend != 1) {
        for (auto&& i : positions) {
            i *= halfExtend;
        }
    }
}

void TriangleMesh::InitAsUVSphere(float radius, uint32_t numberSlices) noexcept {
    uint32_t numberParallels = numberSlices / 2;
    uint32_t numberVertices = (numberParallels + 1) * (numberSlices + 1);
    uint32_t numberIndices = numberParallels * numberSlices * 6;
    float angleStep = (2.0f * std::numbers::pi_v<float>) / numberSlices;
    positions.resize(numberVertices);
    normals.resize(numberVertices);
    uv0.resize(numberVertices);
    tangents.resize(numberVertices);
    for (uint32_t i = 0; i < numberParallels + 1; i++) {
        for (uint32_t j = 0; j < (numberSlices + 1); j++) {
            uint32_t vertexIndex = (i * (numberSlices + 1) + j);
            uint32_t normalIndex = (i * (numberSlices + 1) + j);
            uint32_t texCoordsIndex = (i * (numberSlices + 1) + j);
            uint32_t tangentIndex = (i * (numberSlices + 1) + j);
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
    uint32_t indexIndices = 0;
    indices.resize(numberIndices);
    for (uint32_t i = 0; i < numberParallels; i++) {
        for (uint32_t j = 0; j < (numberSlices); j++) {
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
