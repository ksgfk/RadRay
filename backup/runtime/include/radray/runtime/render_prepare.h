#pragma once

#include <radray/basic_math.h>
#include <radray/nullable.h>

#include <radray/runtime/frame_snapshot.h>
#include <radray/runtime/render_asset_registry.h>

namespace radray::runtime {

struct PreparedDrawItem {
    MeshHandle Mesh{};
    MaterialHandle Material{};
    uint32_t SubmeshIndex{0};
    uint32_t ViewId{0};
    Eigen::Matrix4f LocalToWorld{Eigen::Matrix4f::Identity()};
    Eigen::Vector4f Tint{Eigen::Vector4f::Ones()};
    uint32_t SortKeyHigh{0};
    uint32_t SortKeyLow{0};
};

struct PreparedView {
    uint32_t ViewId{0};
    uint32_t CameraId{0};
    uint32_t OutputWidth{0};
    uint32_t OutputHeight{0};
    const CameraRenderData* Camera{nullptr};
    vector<PreparedDrawItem> DrawItems{};
};

struct PreparedScene {
    vector<PreparedView> Views{};
};

bool PrepareScene(
    const FrameSnapshot& snapshot,
    const RenderAssetRegistry& assets,
    PreparedScene& prepared,
    Nullable<string*> reason = nullptr) noexcept;

}  // namespace radray::runtime
