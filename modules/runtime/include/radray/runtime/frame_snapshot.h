#pragma once

#include <span>

#include <radray/basic_math.h>
#include <radray/nullable.h>
#include <radray/types.h>

#include <radray/runtime/handles.h>

namespace radray::runtime {

struct FrameSnapshotHeader {
    uint64_t FrameId{0};
    uint64_t SimulationTick{0};
    double CpuTimeSeconds{0.0};
    uint32_t ViewCount{0};
    uint32_t CameraCount{0};
    uint32_t MeshBatchCount{0};
    uint32_t Flags{0};
};

struct CameraRenderData {
    uint32_t CameraId{0};
    uint32_t ViewId{0};
    uint32_t Flags{0};
    uint32_t LayerMask{0xFFFFFFFFu};
    float NearPlane{0.1f};
    float FarPlane{1000.0f};
    float VerticalFov{0.0f};
    float AspectRatio{1.0f};
    float Exposure{1.0f};
    Eigen::Matrix4f View{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f Proj{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f ViewProj{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f InvView{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f InvProj{Eigen::Matrix4f::Identity()};
    Eigen::Vector3f WorldPosition{Eigen::Vector3f::Zero()};
    uint32_t OutputWidth{0};
    uint32_t OutputHeight{0};
};

struct VisibleMeshBatch {
    uint64_t InstanceId{0};
    uint32_t ViewMask{0};
    uint32_t Flags{0};
    MeshHandle Mesh{};
    MaterialHandle Material{};
    uint32_t SubmeshIndex{0};
    uint32_t TransformIndex{0};
    Eigen::Matrix4f LocalToWorld{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f PrevLocalToWorld{Eigen::Matrix4f::Identity()};
    Eigen::AlignedBox3f WorldBounds{};
    uint32_t SortKeyHigh{0};
    uint32_t SortKeyLow{0};
};

enum class RenderViewType : uint32_t {
    MainColor = 0,
};

struct RenderViewRequest {
    uint32_t ViewId{0};
    RenderViewType Type{RenderViewType::MainColor};
    uint32_t CameraId{0};
    uint32_t OutputWidth{0};
    uint32_t OutputHeight{0};
    uint32_t Flags{0};
};

class FrameSnapshot {
public:
    FrameSnapshotHeader Header{};
    vector<CameraRenderData> Cameras{};
    vector<VisibleMeshBatch> MeshBatches{};
    vector<RenderViewRequest> Views{};

    std::span<const CameraRenderData> GetCameras() const noexcept;
    std::span<const VisibleMeshBatch> GetMeshBatches() const noexcept;
    std::span<const RenderViewRequest> GetViews() const noexcept;

    bool IsEmpty() const noexcept;
};

class FrameSnapshotBuilder {
public:
    void Reset(uint64_t frameId, uint64_t simulationTick, double cpuTimeSeconds = 0.0) noexcept;

    CameraRenderData& AddCamera();

    VisibleMeshBatch& AddMeshBatch();

    RenderViewRequest& AddView();

    FrameSnapshot Finalize(Nullable<string*> reason = nullptr) noexcept;

private:
    bool Validate(string* reason) const noexcept;

    FrameSnapshot _snapshot{};
};

}  // namespace radray::runtime
