#include <radray/runtime/static_mesh.h>

#include <algorithm>
#include <limits>
#include <cstring>
#include <utility>

#include <array>
#include <fmt/format.h>

#include <radray/triangle_mesh.h>
#include <radray/render/rhi.h>
#include <radray/runtime/gpu_system.h>
#include <radray/wavefront_obj.h>

namespace radray {
namespace {

bool IsByteRangeValid(size_t offset, size_t stride, uint32_t count, size_t elementSize, size_t bufferSize) noexcept {
    if (count == 0 || stride == 0 || elementSize == 0) {
        return false;
    }
    if (offset > bufferSize) {
        return false;
    }
    if (count > 1 && stride > (std::numeric_limits<size_t>::max() - offset) / static_cast<size_t>(count - 1)) {
        return false;
    }

    const size_t lastOffset = offset + static_cast<size_t>(count - 1) * stride;
    if (lastOffset > bufferSize) {
        return false;
    }
    return elementSize <= bufferSize - lastOffset;
}

bool IsVertexBufferEntryValid(const VertexBufferEntry& entry, const MeshResource& meshResource, uint32_t vertexCount) noexcept {
    if (entry.BufferIndex >= meshResource.Bins.size()) {
        return false;
    }

    const uint32_t elementSize = GetVertexDataSizeInBytes(entry.Type, entry.ComponentCount);
    if (elementSize == 0 || entry.Stride == 0) {
        return false;
    }
    if (entry.Offset > entry.Stride || elementSize > entry.Stride - entry.Offset) {
        return false;
    }

    return IsByteRangeValid(
        static_cast<size_t>(entry.Offset),
        static_cast<size_t>(entry.Stride),
        vertexCount,
        static_cast<size_t>(elementSize),
        meshResource.Bins[entry.BufferIndex].GetSize());
}

bool IsIndexBufferEntryValid(const IndexBufferEntry& entry, const MeshResource& meshResource) noexcept {
    if (entry.BufferIndex >= meshResource.Bins.size()) {
        return false;
    }
    if (entry.Stride != sizeof(uint16_t) && entry.Stride != sizeof(uint32_t)) {
        return false;
    }

    return IsByteRangeValid(
        static_cast<size_t>(entry.Offset),
        static_cast<size_t>(entry.Stride),
        entry.IndexCount,
        static_cast<size_t>(entry.Stride),
        meshResource.Bins[entry.BufferIndex].GetSize());
}

bool IsPrimitiveValid(const MeshPrimitive& primitive, const MeshResource& meshResource) noexcept {
    if (primitive.VertexCount == 0 || primitive.VertexBuffers.empty()) {
        return false;
    }
    if (!IsIndexBufferEntryValid(primitive.IndexBuffer, meshResource)) {
        return false;
    }

    for (const VertexBufferEntry& entry : primitive.VertexBuffers) {
        if (!IsVertexBufferEntryValid(entry, meshResource, primitive.VertexCount)) {
            return false;
        }
    }

    return true;
}

bool IsSectionValid(const StaticMeshSection& section, const MeshResource& meshResource) noexcept {
    if (section.PrimitiveIndex >= meshResource.Primitives.size()) {
        return false;
    }
    if (section.IndexCount == 0 || section.MinVertexIndex > section.MaxVertexIndex) {
        return false;
    }

    const MeshPrimitive& primitive = meshResource.Primitives[section.PrimitiveIndex];
    if (section.FirstIndex > primitive.IndexBuffer.IndexCount) {
        return false;
    }
    if (section.IndexCount > primitive.IndexBuffer.IndexCount - section.FirstIndex) {
        return false;
    }
    return section.MaxVertexIndex < primitive.VertexCount;
}

bool IsObjIndexValid(int32_t index, size_t count, bool optional) noexcept {
    if (index == 0) {
        return optional;
    }
    if (index > 0) {
        return static_cast<size_t>(index) <= count;
    }
    const int64_t magnitude = -static_cast<int64_t>(index);
    return magnitude <= static_cast<int64_t>(count);
}

bool AreObjFaceIndicesValid(const WavefrontObjReader& reader) noexcept {
    for (const WavefrontObjFace& face : reader.Faces()) {
        const int32_t positions[]{face.V1, face.V2, face.V3};
        const int32_t normals[]{face.Vn1, face.Vn2, face.Vn3};
        const int32_t uvs[]{face.Vt1, face.Vt2, face.Vt3};
        for (int32_t index : positions) {
            if (!IsObjIndexValid(index, reader.Positions().size(), false)) {
                return false;
            }
        }
        for (int32_t index : normals) {
            if (!IsObjIndexValid(index, reader.Normals().size(), true)) {
                return false;
            }
        }
        for (int32_t index : uvs) {
            if (!IsObjIndexValid(index, reader.UVs().size(), true)) {
                return false;
            }
        }
    }
    return true;
}

bool BuildDefaultSectionsAndBounds(
    const MeshResource& meshResource,
    vector<StaticMeshSection>& sections,
    Eigen::Vector3f& boundsMin,
    Eigen::Vector3f& boundsMax) noexcept {
    sections.clear();
    boundsMin = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    boundsMax = Eigen::Vector3f::Constant(std::numeric_limits<float>::lowest());
    bool hasPosition = false;
    for (uint32_t primitiveIndex = 0;
         primitiveIndex < meshResource.Primitives.size();
         ++primitiveIndex) {
        const MeshPrimitive& primitive = meshResource.Primitives[primitiveIndex];
        sections.emplace_back(
            primitiveIndex,
            0,
            primitive.IndexBuffer.IndexCount,
            0,
            primitive.VertexCount - 1);
        const auto position = std::find_if(
            primitive.VertexBuffers.begin(),
            primitive.VertexBuffers.end(),
            [](const VertexBufferEntry& entry) noexcept {
                return entry.Semantic == VertexSemantics::POSITION &&
                       entry.SemanticIndex == 0 &&
                       entry.Type == VertexDataType::FLOAT &&
                       entry.ComponentCount >= 3;
            });
        if (position == primitive.VertexBuffers.end() ||
            position->BufferIndex >= meshResource.Bins.size()) {
            return false;
        }
        const std::span<const byte> data =
            meshResource.Bins[position->BufferIndex].GetData();
        for (uint32_t vertexIndex = 0;
             vertexIndex < primitive.VertexCount;
             ++vertexIndex) {
            const uint64_t offset = static_cast<uint64_t>(position->Offset) +
                                    static_cast<uint64_t>(vertexIndex) *
                                        position->Stride;
            if (offset > data.size() || sizeof(float) * 3 > data.size() - offset) {
                return false;
            }
            float values[3];
            std::memcpy(values, data.data() + offset, sizeof(values));
            const Eigen::Vector3f point{values[0], values[1], values[2]};
            boundsMin = boundsMin.cwiseMin(point);
            boundsMax = boundsMax.cwiseMax(point);
            hasPosition = true;
        }
    }
    return hasPosition && IsStaticMeshDataValid(meshResource, sections);
}

}  // namespace

StaticMeshSection::StaticMeshSection() noexcept
    : PrimitiveIndex(0),
      FirstIndex(0),
      IndexCount(0),
      MinVertexIndex(0),
      MaxVertexIndex(0),
      VertexOffset(0) {
}

StaticMeshSection::StaticMeshSection(
    uint32_t primitiveIndex,
    uint32_t firstIndex,
    uint32_t indexCount,
    uint32_t minVertexIndex,
    uint32_t maxVertexIndex,
    int32_t vertexOffset) noexcept
    : PrimitiveIndex(primitiveIndex),
      FirstIndex(firstIndex),
      IndexCount(indexCount),
      MinVertexIndex(minVertexIndex),
      MaxVertexIndex(maxVertexIndex),
      VertexOffset(vertexOffset) {
}

bool IsStaticMeshDataValid(
    const MeshResource& meshResource,
    std::span<const StaticMeshSection> sections) noexcept {
    if (meshResource.Primitives.empty()) {
        return false;
    }
    for (const MeshPrimitive& primitive : meshResource.Primitives) {
        if (!IsPrimitiveValid(primitive, meshResource)) {
            return false;
        }
    }
    for (const StaticMeshSection& section : sections) {
        if (!IsSectionValid(section, meshResource)) {
            return false;
        }
    }
    return true;
}

StaticMesh::StaticMesh(
    MeshResource meshResource,
    vector<StaticMeshSection> sections,
    const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax,
    GpuMesh renderMesh) noexcept
    : _meshResource(std::move(meshResource)),
      _sections(std::move(sections)),
      _boundsMin(boundsMin),
      _boundsMax(boundsMax),
      _renderMesh(std::move(renderMesh)) {
}

StaticMesh::~StaticMesh() noexcept = default;

bool StaticMesh::IsValid() const noexcept {
    return IsStaticMeshDataValid(_meshResource, _sections);
}

void StaticMesh::OnUnload(AssetManager& manager) {
    // 【必须延迟】: SceneProxy 缓存 GpuMesh::DrawData* 并录进命令列表 (见
    // primitive_scene_proxy.h), 那些 buffer 要活到 fence 之后。
    //
    // 【整包交出】: buffer 之间无相互依赖, 但整包交出的形状让"销毁顺序在哪里表达"这件事
    // 在所有资产上一致, 而不是每种资产各自决定。见 AssetManager::DeferDestroy。
    manager.DeferDestroy([mesh = std::move(_renderMesh)]() noexcept {});
    _renderMesh = GpuMesh{};
}

RuntimeTypeId StaticMesh::GetTypeId() const noexcept {
    return runtime_type_id_v<StaticMesh>;
}

task<AssetLoadResult> LoadStaticMesh(
    FrameUploadScheduler& frameUploads,
    MeshResource meshResource) {
    // 阶段(均为协程内部事务):
    //  1) CPU 校验网格数据。
    //  2) 两阶段 GPU 上传:co_await FrameUploadScheduler::BeginUpload 挂起至帧顶拿 cmd/uploader,
    //     inline 录制 copy 进当前帧 cmdbuffer,再 co_await WaitGpu 跨帧等 fence。
    //  3) 一次性构造内容与资产。
    // 【校验先于上传】: 无效数据不该占用 upload 带宽, 也不该建出半成品内容。
    if (!IsStaticMeshDataValid(meshResource, {})) {
        co_return AssetLoadResult::Failure("static mesh resource is invalid");
    }
    vector<StaticMeshSection> sections;
    Eigen::Vector3f boundsMin;
    Eigen::Vector3f boundsMax;
    if (!BuildDefaultSectionsAndBounds(
            meshResource,
            sections,
            boundsMin,
            boundsMax)) {
        co_return AssetLoadResult::Failure("static mesh sections or bounds are invalid");
    }

    // GPU 上传:两阶段 await(无 callback)。BeginUpload 挂起至帧顶拿到 cmd/uploader,
    // 在本协程里 inline 录制 copy,再 co_await WaitGpu 等该 flight 的 fence。
    FrameUploadScope frame = co_await frameUploads.BeginUpload();
    std::optional<GpuMesh> renderMesh =
        frame.GetUploader().UploadMeshResource(frame.GetCommandBuffer(), meshResource);
    if (!renderMesh.has_value()) {
        co_return AssetLoadResult::Failure("static mesh upload recording failed");
    }
    co_await frame.WaitGpu();

    co_return AssetLoadResult::Success(make_unique<StaticMesh>(
        std::move(meshResource),
        std::move(sections),
        boundsMin,
        boundsMax,
        std::move(renderMesh.value())));
}

MeshImporter::MeshImporter(FrameUploadScheduler& frameUploads) noexcept
    : _frameUploads(frameUploads) {
}

std::string_view MeshImporter::GetTypeName() const noexcept {
    return "mesh";
}

std::span<const std::string_view> MeshImporter::GetFileExtensions() const noexcept {
    static constexpr std::array<std::string_view, 1> extensions{".obj"};
    return extensions;
}

task<AssetLoadResult> MeshImporter::Load(const AssetLoadContext& ctx) {
    return LoadMesh(&_frameUploads, ctx.AbsolutePath);
}

task<AssetLoadResult> MeshImporter::LoadMesh(
    FrameUploadScheduler* frameUploads,
    std::filesystem::path path) {
    WavefrontObjReader reader{path};
    reader.Read();
    if (reader.HasError()) {
        co_return AssetLoadResult::Failure(fmt::format(
            "cannot parse mesh source '{}': {}",
            path.string(),
            reader.Error()));
    }
    if (reader.Faces().empty() || !AreObjFaceIndicesValid(reader)) {
        co_return AssetLoadResult::Failure(fmt::format(
            "mesh source '{}' has no valid triangle faces",
            path.string()));
    }

    TriangleMesh triangleMesh;
    reader.ToTriangleMesh(&triangleMesh);
    if (!triangleMesh.IsValid()) {
        co_return AssetLoadResult::Failure(fmt::format(
            "mesh source '{}' produced inconsistent vertex attributes",
            path.string()));
    }

    MeshResource meshResource;
    triangleMesh.ToSimpleMeshResource(&meshResource);
    co_return co_await LoadStaticMesh(*frameUploads, std::move(meshResource));
}

}  // namespace radray
