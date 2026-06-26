#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include <radray/basic_math.h>
#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/render/scene.h>
#include <radray/runtime/render/scene_view.h>

// gltf_viewer 私有的灯光/阴影代码:从旧 radray::SceneLightBuffer / scene_renderer 阴影助手
// 迁出,改吃 srp::Scene / srp::Light / srp::SceneView。放在独立的 gltfviewer 命名空间,
// 避免与仍编译进 radrayruntime 的旧 radray:: 同名符号在链接期冲突(ODR)。
namespace gltfviewer {

namespace srp = radray::srp;
namespace render = radray::render;
using radray::unique_ptr;
using radray::vector;
using radray::string;

// ── GPU 端结构(布局与 gltf_viewer.hlsl 的 space0 约定逐字一致)──

struct DirectionalLightGpu {
    float Direction[4];
    float Irradiance[4];
};

struct PointLightGpu {
    float Position[4];   // xyz position, w = range
    float Intensity[4];  // rgb radiance, w = shadow first-slice index (-1 = no shadow)
};

struct SpotLightGpu {
    float Position[4];   // xyz position, w = range
    float Direction[4];  // xyz normalized spot direction, w = cos(outer half-angle)
    float Intensity[4];  // rgb radiance, w = cos(inner half-angle)
    float Params[4];     // x = shadow slice index (-1 = no shadow), yzw reserved
};

struct LightInfoGpu {
    uint32_t Counts[4];  // x directional, y point, z spot
};

static constexpr uint32_t MaxShadowCascadesGpu = 4;          // Must match ShadowCascadeData::MaxCascades / shader.
static constexpr uint32_t MaxAdditionalShadowSlicesGpu = 16; // Must match AdditionalShadowData::MaxSlices / shader.

struct ShadowParamGpu {
    float WorldToShadow[MaxShadowCascadesGpu][16];
    float CascadeSphere[MaxShadowCascadesGpu][4];
    float Params[4];
};

struct AdditionalShadowParamGpu {
    float WorldToShadow[MaxAdditionalShadowSlicesGpu][16];
    float Params[4]; // x enable, y atlas size(px), z slice count, w soft mode
};

static_assert(sizeof(DirectionalLightGpu) == 32);
static_assert(sizeof(PointLightGpu) == 32);
static_assert(sizeof(SpotLightGpu) == 64);
static_assert(sizeof(LightInfoGpu) == 16);
static_assert(sizeof(ShadowParamGpu) == 336);
static_assert(sizeof(AdditionalShadowParamGpu) == MaxAdditionalShadowSlicesGpu * 64 + 16);

// ── CPU 端阴影结构 ──

/// 单个方向光级联阴影的渲染参数。
struct ShadowCascade {
    srp::SceneView View{};
    float DepthBias{0.0f};
    float NormalBias{0.0f};
};

/// 软阴影 PCF 档位。值与 shader 端 gShadowParam.Params.w 一致。
enum class ShadowSoftMode : uint32_t {
    Hard = 0,
    Low = 1,
    Medium = 2,
};

/// 一帧的方向光级联阴影数据。
struct ShadowCascadeData {
    static constexpr uint32_t MaxCascades = 4;

    std::array<ShadowCascade, MaxCascades> Cascades{};
    std::array<Eigen::Vector3f, MaxCascades> SphereCenters{};
    std::array<float, MaxCascades> SphereRadiiSq{};
    Eigen::Vector3f LightDirectionForBias{0.0f, -1.0f, 0.0f};
    uint32_t CascadeCount{0};
    bool Enabled{false};
    ShadowSoftMode SoftMode{ShadowSoftMode::Medium};
};

/// 附加光(聚光/点光)阴影的一个 slice(对应图集 Texture2DArray 的一层)。
struct AdditionalShadowSlice {
    srp::SceneView View{};
    Eigen::Vector3f LightDirectionForBias{0.0f, -1.0f, 0.0f};
    float DepthBias{0.0f};
    float NormalBias{0.0f};
};

/// 附加光阴影类型。值与 shader 端约定一致。
enum class AdditionalShadowKind : uint32_t {
    Spot = 0,
    Point = 1,
};

/// 一盏投射阴影的附加光及其在图集中的 slice 范围。
struct AdditionalShadowLight {
    const srp::Light* Light{nullptr};
    AdditionalShadowKind Kind{AdditionalShadowKind::Spot};
    uint32_t FirstSlice{0};
    uint32_t SliceCount{0};
    Eigen::Vector3f Position{Eigen::Vector3f::Zero()};
    Eigen::Vector3f Direction{0.0f, -1.0f, 0.0f};
    float Range{0.0f};
};

/// 一帧的附加光阴影数据。
struct AdditionalShadowData {
    static constexpr uint32_t MaxSlices = 16;
    static constexpr uint32_t PointFaceCount = 6;

    std::array<AdditionalShadowSlice, MaxSlices> Slices{};
    vector<AdditionalShadowLight> Lights{};
    uint32_t SliceCount{0};
    uint32_t Resolution{1024};
    bool Enabled{false};
    ShadowSoftMode SoftMode{ShadowSoftMode::Medium};

    void Clear() noexcept {
        Lights.clear();
        SliceCount = 0;
        Enabled = false;
    }
};

// ── 阴影构建助手(从 scene_renderer.cpp 迁出)──

/// 软阴影 PCF 核半径(纹素)。Hard=1.0,Low=1.5,Medium=2.5。
float AdditionalShadowKernelRadius(ShadowSoftMode mode) noexcept;

/// 点光第 face 面(0..5 = +X,-X,+Y,-Y,+Z,-Z)的前向。与 shader cube_face_id 约定一致。
Eigen::Vector3f PointShadowFaceForward(uint32_t face) noexcept;

/// 点光第 face 面的 up 向量(用于构建 LookAt)。
Eigen::Vector3f PointShadowFaceUp(uint32_t face) noexcept;

/// 构建聚光的单个阴影 slice。
AdditionalShadowSlice BuildSpotShadowSlice(
    const Eigen::Vector3f& position,
    const Eigen::Vector3f& direction,
    float outerAngle,
    float range,
    uint32_t resolution,
    float depthBiasTexels,
    float normalBiasTexels,
    ShadowSoftMode softMode) noexcept;

/// 构建点光第 face 面的阴影 slice(强制 normalBias=0)。
AdditionalShadowSlice BuildPointShadowFaceSlice(
    const Eigen::Vector3f& position,
    uint32_t face,
    float range,
    uint32_t resolution,
    float depthBiasTexels,
    ShadowSoftMode softMode) noexcept;

/// 从场景收集全部投射阴影的附加光,分配图集 slice,填充 out。
bool BuildAdditionalShadows(
    const srp::Scene& scene,
    uint32_t resolution,
    ShadowSoftMode softMode,
    AdditionalShadowData& out);

// ── 灯光缓冲(space0):构建并写入 per-view descriptor set ──

class SceneLightBuffer {
public:
    static constexpr uint32_t MaxDirectionalLights = 8;
    static constexpr uint32_t MaxPointLights = 64;
    static constexpr uint32_t MaxSpotLights = 64;
    static constexpr uint32_t MaxShadowCascadesGpu = gltfviewer::MaxShadowCascadesGpu;
    static constexpr uint32_t MaxAdditionalShadowSlicesGpu = gltfviewer::MaxAdditionalShadowSlicesGpu;
    static constexpr uint32_t MaxFlights = 3;

    SceneLightBuffer() noexcept = default;
    SceneLightBuffer(const SceneLightBuffer&) = delete;
    SceneLightBuffer(SceneLightBuffer&&) = delete;
    SceneLightBuffer& operator=(const SceneLightBuffer&) = delete;
    SceneLightBuffer& operator=(SceneLightBuffer&&) = delete;
    ~SceneLightBuffer() noexcept;

    render::DescriptorSet* Update(
        render::Device& device,
        render::RootSignature* rootSig,
        render::DescriptorSetIndex viewSet,
        uint32_t flightIndex,
        const srp::Scene& scene,
        const ShadowCascadeData& shadow,
        render::TextureView* shadowArraySrv,
        const AdditionalShadowData& additionalShadow,
        render::TextureView* additionalShadowArraySrv);

    void Clear() noexcept;

private:
    struct FlightResources {
        render::RootSignature* RootSig{nullptr};
        render::DescriptorSetIndex ViewSet{0};
        unique_ptr<render::Buffer> DirectionalBuffer;
        unique_ptr<render::Buffer> PointBuffer;
        unique_ptr<render::Buffer> SpotBuffer;
        unique_ptr<render::Buffer> InfoBuffer;
        unique_ptr<render::Buffer> ShadowBuffer;
        unique_ptr<render::Buffer> AdditionalShadowBuffer;
        unique_ptr<render::Sampler> ShadowCmpSampler;
        unique_ptr<render::DescriptorSet> DescriptorSet;
    };

    bool EnsureFlightResources(
        render::Device& device,
        render::RootSignature* rootSig,
        render::DescriptorSetIndex viewSet,
        FlightResources& resources,
        uint32_t flightIndex);

    static bool WriteBuffer(render::Buffer& buffer, const void* data, uint64_t size, std::string_view debugName);
    static bool WriteDescriptorSetResources(
        FlightResources& resources,
        render::TextureView* shadowArraySrv,
        render::TextureView* additionalShadowArraySrv);

    std::array<FlightResources, MaxFlights> _flights{};
};

}  // namespace gltfviewer
