#include <radray/runtime/static_mesh.h>

#include <limits>
#include <utility>

#include <radray/render/rhi.h>
#include <radray/runtime/gpu_system.h>

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

StaticMeshContent::StaticMeshContent(
    AssetContentKey key,
    IRenderResourceRecycler& recycler,
    MeshResource meshResource,
    vector<StaticMeshSection> sections,
    const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax,
    GpuMesh renderMesh) noexcept
    : AssetContent(key, recycler),
      _meshResource(std::move(meshResource)),
      _sections(std::move(sections)),
      _boundsMin(boundsMin),
      _boundsMax(boundsMax),
      _renderMesh(std::move(renderMesh)) {
}

StaticMeshContent::~StaticMeshContent() noexcept = default;

bool StaticMeshContent::IsValid() const noexcept {
    return IsStaticMeshDataValid(_meshResource, _sections);
}

void StaticMeshContent::ReleaseRenderResources(IRenderResourceRecycler& recycler) noexcept {
    // 【无条件走 recycler】: 分离前这里有一句 use_count() == 1, 于是"别人还持有"时 buffer
    // 会绕过 recycler 直接析构 —— 不等 fence。归零由基类接管后, 到这里必然是唯一所有者。
    for (auto& buffer : _renderMesh.Buffers) {
        recycler.RecycleRenderResource(std::move(buffer));
    }
    _renderMesh = GpuMesh{};
    _meshResource = MeshResource{};
    _sections.clear();
}

StaticMesh::StaticMesh(StaticMeshContentRef content) noexcept
    : _content(std::move(content)) {
}

StaticMesh::~StaticMesh() noexcept = default;

void StaticMesh::OnUnload(IRenderResourceRecycler& recycler) {
    // 【只放开槽位那份引用】: GPU buffer 的回收归内容归零时做, 此刻可能还有 SceneProxy
    // 在用它录制。
    (void)recycler;
    _content.Reset();
}

AssetTypeId StaticMesh::GetTypeId() const noexcept {
    return runtime_type_id_v<StaticMesh>;
}

AssetLoadTask LoadStaticMesh(
    AssetManager& assetManager,
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

    // GPU 上传:两阶段 await(无 callback)。BeginUpload 挂起至帧顶拿到 cmd/uploader,
    // 在本协程里 inline 录制 copy,再 co_await WaitGpu 等该 flight 的 fence。
    FrameUploadScope frame = co_await frameUploads.BeginUpload();
    std::optional<GpuMesh> renderMesh =
        frame.GetUploader().UploadMeshResource(frame.GetCommandBuffer(), meshResource);
    if (!renderMesh.has_value()) {
        co_return AssetLoadResult::Failure("static mesh upload recording failed");
    }
    co_await frame.WaitGpu();

    // 内容必须经 AssetManager 创建 —— recycler 由那里注入, 见 AssetContentKey。
    StaticMeshContentRef content = assetManager.MakeContent<StaticMeshContent>(
        std::move(meshResource),
        vector<StaticMeshSection>{},
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f::Zero(),
        std::move(renderMesh.value()));
    co_return AssetLoadResult::Success(make_unique<StaticMesh>(std::move(content)));
}

}  // namespace radray
