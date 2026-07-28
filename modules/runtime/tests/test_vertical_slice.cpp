// 垂直切片: 从 shader manifest 一路走到真实 GPU 绘制, 再把像素读回来断言。
//
// 【为什么存在】: shader_asset 的底层 (manifest 解析、变体域、artifact 索引、cook) 有大量
// 单元测试, 但 CreateShader / CreatePipelineLayout / CreateShaderParameterSet /
// CreateGraphicsPipelineState 这几个"把 manifest 变成 PSO"的接口, 在本文件之前于
// modules/runtime 下没有任何调用方 —— 整条链从未被端到端跑过一次。本文件就是那第一次。
//
// 【为什么不做成 example】: examples/ 的两个 demo 依赖已被删除的 material 层, 要跑起来
// 得先把那层补出来; 而那层的 API 形状恰恰是这条切片应当用来推导的东西。先补再验证等于
// 用猜出来的形状验证自己。headless 还额外换来自动回归。
//
// 【刻意不碰的东西】: swapchain / present / Scene → SceneProxy → 绘制提交。前者是另一个
// 已在运行的子系统; 后者目前不可能产出任何 proxy —— PrimitiveComponent::CreateSceneProxy
// 基类返回 nullptr (src/components/primitive_component.cpp:42-44), Scene::AddPrimitive
// 拿到 nullptr 即返回 (src/render_framework/scene.cpp:19-22)。补齐它需要先决定
// material/mesh 层的形状, 那是切片之后的事。
//
// 【用 error_pass 而非 forward_pass】: error_pass 顶点只有 POSITION、PSMain 返回洋红常量、
// 无材质绑定, 断言"像素是洋红"最干净。forward_pass 要填 ViewConstants 大结构 + 3 个
// binding group + 48 字节顶点, 绝大部分工作与本切片要验证的东西无关。
//
// 【JIT / AOT 双参数化】: 同一条链路跑两遍, 唯一区别是字节码从哪来。
// - Jit: 无产物, resolver 现场编译 (开发构建)。
// - Aot: 先 CookShaderAssetFile, 再用 AllowJit == false + dxc == nullptr 解析 (发布包)。
// 后者是 radray_shader_cook 在构建期做的事的等价物。分开两个参数而不是只测 AOT, 是因为
// 两条路径在 ShaderResolver 里几乎不共享代码 —— AOT 那半段 (toolchain 比对、按源文件取
// cook 时身份、算 key、读 blob 自验) 只在有产物时才会执行。

#include <radray/basic_math.h>
#include <radray/environment.h>
#include <radray/logger.h>
#include <radray/render/dxc.h>
#include <radray/render/rhi.h>
#include <radray/runtime/shader_manifest.h>
#include <radray/types.h>
#include <radray/utility.h>

#include <gtest/gtest.h>

#include <fmt/format.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>

namespace radray {
namespace {

constexpr uint32_t kTargetWidth = 64;
constexpr uint32_t kTargetHeight = 64;
constexpr render::TextureFormat kTargetFormat = render::TextureFormat::RGBA8_UNORM;

/// 仓库根。环境变量 (ctest 注入) 优先, 缺失时回退配置期编进来的路径,
/// 使本用例在直接跑 exe (调试器、手动 --gtest_filter) 时同样可用。
std::filesystem::path GetProjectRoot() {
    const string fromEnv = GetEnv("RADRAY_PROJECT_DIR");
    if (!fromEnv.empty()) {
        return std::filesystem::path{fromEnv};
    }
#if defined(RADRAY_PROJECT_DIR_DEFAULT)
    return std::filesystem::path{RADRAY_PROJECT_DIR_DEFAULT};
#else
    return {};
#endif
}

/// error_pass 的 gPerObject。对应 shaderlib/forward_pipeline/bindings.hlsli 的
/// ObjectConstants。
struct ObjectConstantsCpu {
    std::array<float, 16> ObjectToWorld{};
};

// gView 的 CPU 侧镜像。error_pass 的 VS 只读 ViewProj, 但 cbuffer 必须按完整结构
// 分配 —— 尺寸不足时 D3D12 会在建 CBV 时越界。
//
// 【为何镜像整个结构而不是直接写个够大的数字】: 这些上限来自 shaderlib 的宏, 改了
// 之后一个手写的字节数不会有任何提示。镜像结构至少让 ViewProj 必须在偏移 0 这条
// 前提是显式的; 若哪天 shaderlib 往 ViewConstants 前面插了字段, 下面的
// static_assert 不会响, 但绘制会立刻变黑, 比静默读到垃圾数据好。
constexpr uint32_t kMaxPointLights = 8;        // lighting/lights.hlsli:12
constexpr uint32_t kMaxDirectionalLights = 8;  // lighting/lights.hlsli:11
constexpr uint32_t kCubeFaceCount = 6;         // shadow/cube.hlsli:16
constexpr uint32_t kMaxCascades = 4;           // shadow/cascade.hlsli:17

using Float4 = std::array<float, 4>;
using Float4x4 = std::array<float, 16>;

struct PointLightCpu {
    Float4 Position{};
    Float4 Intensity{};
};

struct DirectionalLightCpu {
    Float4 Direction{};
    Float4 Irradiance{};
};

struct CubeShadowCpu {
    std::array<Float4x4, kCubeFaceCount> ViewProj{};
    Float4 LightPositionInvRadius{};
    Float4 Params{};
};

struct CascadeShadowCpu {
    std::array<Float4x4, kMaxCascades> WorldToShadow{};
    std::array<Float4, kMaxCascades> CascadeSphere{};
    std::array<Float4, kMaxCascades> CascadeBias{};
    Float4 Params{};
};

/// 对应 shaderlib/forward_pipeline/view.hlsli:13-26 的 ViewConstants。
struct ViewConstantsCpu {
    Float4x4 ViewProj{};
    Float4 CameraPosition{};
    std::array<uint32_t, 4> LightCounts{};
    std::array<PointLightCpu, kMaxPointLights> PointLights{};
    std::array<DirectionalLightCpu, kMaxDirectionalLights> DirectionalLights{};
    CubeShadowCpu PointShadow{};
    CascadeShadowCpu DirectionalShadow{};
};

// HLSL 的 float4/float4x4 在 cbuffer 里是 16 字节对齐且紧密排布, 与上面这组
// std::array 布局一致, 故整体尺寸应当逐字段吻合。
static_assert(sizeof(ViewConstantsCpu) == 1424, "ViewConstants layout drifted from view.hlsli");
static_assert(offsetof(ViewConstantsCpu, ViewProj) == 0, "the VS reads ViewProj at offset 0");

/// 单位矩阵。切片刻意让顶点直接给裁剪空间坐标, 两个矩阵都填单位矩阵 ——
/// 这样断言的是"整条 shader 链路通了", 而不是"矩阵数学对不对"。
std::array<float, 16> MakeIdentity() {
    return {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
}

/// 一个覆盖视口中心的三角形, 坐标已在 NDC。
/// 足够大, 使中心像素必定被覆盖, 同时四角必定不被覆盖 —— 后者用来验证我们真的在看
/// 光栅化结果, 而不是把 clear 颜色当成了绘制结果。
constexpr std::array<float, 9> kTriangleVertices{
    0.0f, 0.8f, 0.0f,
    -0.8f, -0.8f, 0.0f,
    0.8f, -0.8f, 0.0f};

constexpr std::array<uint16_t, 3> kTriangleIndices{0, 1, 2};

/// 一个后端的全部 GPU 对象。析构顺序按声明逆序, 故成员顺序即依赖顺序。
struct SliceContext {
    // Vulkan 的 instance 是进程级全局, 必须最后关。
    bool VulkanEnvInitialized{false};
    unique_ptr<render::DXGIFactory> Factory;
    shared_ptr<render::Device> Device;
    render::CommandQueue* Queue{nullptr};

    ~SliceContext() {
        // 先放掉设备再关 Vulkan 全局环境, 否则 instance 先死会带走设备。
        Device.reset();
        Factory.reset();
#if defined(RADRAY_ENABLE_VULKAN)
        if (VulkanEnvInitialized) {
            render::InstanceVulkan::ShutdownEnv();
        }
#endif
    }
};

/// 创建设备。返回 false 表示该后端在当前机器上不可用 (无显卡、无驱动、CI 无 GPU),
/// 调用方应 GTEST_SKIP 而非失败。
bool TryCreateDevice(render::RenderBackend backend, SliceContext& ctx) {
    if (backend == render::RenderBackend::D3D12) {
#if defined(RADRAY_ENABLE_D3D12)
        render::DXGIFactoryDescriptor factoryDesc{};
        factoryDesc.IsEnableDebugLayer = false;
        auto factory = render::DXGIFactory::Create(factoryDesc);
        if (!factory.HasValue()) {
            return false;
        }
        ctx.Factory = factory.Release();

        render::D3D12DeviceDescriptor d3d12Desc{};
        d3d12Desc.Factory = ctx.Factory.get();
        auto device = render::Device::Create(render::DeviceDescriptor{d3d12Desc});
        if (!device.HasValue()) {
            return false;
        }
        ctx.Device = device.Release();
#else
        return false;
#endif
    } else {
#if defined(RADRAY_ENABLE_VULKAN)
        render::VulkanInstanceDescriptor instanceDesc{};
        instanceDesc.AppName = "radray_vertical_slice";
        instanceDesc.EngineName = "radray";
        instanceDesc.IsEnableDebugLayer = false;
        auto instance = render::InstanceVulkan::InitEnv(instanceDesc);
        if (!instance.HasValue()) {
            return false;
        }
        ctx.VulkanEnvInitialized = true;

        // Vulkan 的队列必须在建设备时预先申请 —— 与 D3D12 的惰性创建不同,
        // GetCommandQueue 只会返回这里声明过的。
        const std::array<render::VulkanCommandQueueDescriptor, 1> queues{
            render::VulkanCommandQueueDescriptor{render::QueueType::Direct, 1}};
        render::VulkanDeviceDescriptor vkDesc{};
        vkDesc.Queues = queues;
        auto device = render::Device::Create(render::DeviceDescriptor{vkDesc});
        if (!device.HasValue()) {
            return false;
        }
        ctx.Device = device.Release();
#else
        return false;
#endif
    }

    auto queue = ctx.Device->GetCommandQueue(render::QueueType::Direct, 0);
    if (!queue.HasValue()) {
        return false;
    }
    ctx.Queue = queue.Unwrap();
    return true;
}

/// 建一个 Upload 堆 buffer 并把数据写进去。
///
/// Upload 堆可以直接当 VB/IB/CB 用, 不需要再 copy 到 Device 堆 —— 切片要验证的是
/// shader 链路, 省掉一次 staging 让噪音更少。
Nullable<unique_ptr<render::Buffer>> MakeUploadBuffer(
    render::Device& device,
    std::span<const byte> data,
    render::BufferUses usage) {
    render::BufferDescriptor desc{
        .Size = data.size(),
        .Memory = render::MemoryType::Upload,
        .Usage = usage | render::BufferUse::MapWrite,
        .Hints = render::ResourceHint::None};
    auto buffer = device.CreateBuffer(desc);
    if (!buffer.HasValue()) {
        return nullptr;
    }
    unique_ptr<render::Buffer> result = buffer.Release();
    void* mapped = result->Map(0, data.size());
    if (mapped == nullptr) {
        return nullptr;
    }
    std::memcpy(mapped, data.data(), data.size());
    result->FlushMappedRange(render::BufferRange{0, data.size()});
    result->Unmap();
    return result;
}

/// 字节码从哪来。
enum class SliceBytecodeMode {
    /// 开发构建: 没有产物, resolver 现场编译。
    Jit,
    /// 发布包: 先 cook, 再用 AllowJit == false + dxc == nullptr 解析。
    Aot,
};

struct SliceParams {
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    SliceBytecodeMode Mode{SliceBytecodeMode::Jit};
};

/// INSTANTIATE_TEST_SUITE_P 的参数列表里不能写 `SliceParams{a, b}` —— 那个逗号会被
/// 预处理器当成宏参数分隔符。
constexpr SliceParams MakeSliceParams(
    render::RenderBackend backend,
    SliceBytecodeMode mode) noexcept {
    return SliceParams{backend, mode};
}

/// 一个临时目录, 内含 manifest 的副本。
///
/// 【为何要拷一份】: cook 把产物写到 manifest 旁边 (GetShaderArtifactDirectory), 直接烘
/// 仓库里那份会往源码树塞产物目录。ShaderRoot 仍指向仓库的 shaderlib —— 源码要从那里
/// 读, 只有产物需要落在别处, 而这两者本来就是分开的参数。
class ScopedCookedManifest {
public:
    ScopedCookedManifest() {
        static std::atomic<uint32_t> counter{0};
        std::error_code error;
        const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(error);
        if (error) {
            return;
        }
        _dir = tempRoot / fmt::format(
                              "radray_slice_cook_{}_{}",
                              std::chrono::steady_clock::now().time_since_epoch().count(),
                              counter.fetch_add(1));
        std::filesystem::create_directories(_dir, error);
        if (error) {
            _dir.clear();
        }
    }
    ~ScopedCookedManifest() {
        std::error_code error;
        if (!_dir.empty()) {
            std::filesystem::remove_all(_dir, error);
        }
    }
    ScopedCookedManifest(const ScopedCookedManifest&) = delete;
    ScopedCookedManifest& operator=(const ScopedCookedManifest&) = delete;

    bool IsValid() const noexcept { return !_dir.empty(); }
    std::filesystem::path ManifestPath() const { return _dir / "error_pass.shader.json"; }

    bool CopyFrom(const std::filesystem::path& source) const {
        std::error_code error;
        std::filesystem::copy_file(
            source, ManifestPath(), std::filesystem::copy_options::overwrite_existing, error);
        return !error;
    }

private:
    std::filesystem::path _dir;
};

class VerticalSliceTest : public testing::TestWithParam<SliceParams> {};

TEST_P(VerticalSliceTest, ManifestToPixels) {
    const render::RenderBackend backend = GetParam().Backend;
    const SliceBytecodeMode mode = GetParam().Mode;

    const std::filesystem::path projectRoot = GetProjectRoot();
    ASSERT_FALSE(projectRoot.empty()) << "the project root is unknown";
    const std::filesystem::path shaderRoot = projectRoot / "shaderlib";
    const std::filesystem::path sourceManifestPath =
        shaderRoot / "forward_pipeline" / "error_pass.shader.json";
    ASSERT_TRUE(std::filesystem::is_regular_file(sourceManifestPath));

    // AOT 模式下解析的是临时目录里的副本, 产物落在它旁边。
    ScopedCookedManifest cookWorkspace;
    std::filesystem::path manifestPath = sourceManifestPath;
    if (mode == SliceBytecodeMode::Aot) {
        ASSERT_TRUE(cookWorkspace.IsValid());
        ASSERT_TRUE(cookWorkspace.CopyFrom(sourceManifestPath));
        manifestPath = cookWorkspace.ManifestPath();
    }

    // ---- 阶段 1: 设备 ----
    SliceContext ctx;
    if (!TryCreateDevice(backend, ctx)) {
        GTEST_SKIP() << "the render backend is unavailable on this machine";
    }
    render::Device& device = *ctx.Device;

    auto dxcResult = render::CreateDxc();
    if (!dxcResult.HasValue()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    shared_ptr<render::Dxc> dxc = dxcResult.Release();

    // 后端决定字节码类型: D3D12 只收 DXIL, Vulkan 只收 SPIRV。
    const render::ShaderBlobCategory category =
        backend == render::RenderBackend::D3D12
            ? render::ShaderBlobCategory::DXIL
            : render::ShaderBlobCategory::SPIRV;

    // AOT: 先烘, 后面用"发布包配置"解析。这里就是构建期 radray_shader_cook 干的事,
    // 只是范围收窄到本后端需要的那一种字节码。
    if (mode == SliceBytecodeMode::Aot) {
        const vector<render::ShaderBlobCategory> categories{category};
        const ShaderCookOptions cookOptions{
            .ShaderRoot = shaderRoot,
            .ManifestPath = manifestPath,
            .Categories = categories,
            .ValidateReflection = true,
            .Incremental = false};
        const ShaderCookResult cook = CookShaderAssetFile(*dxc, cookOptions);
        string cookErrors;
        for (const ShaderAssetDiagnostic& diagnostic : cook.Diagnostics) {
            if (!cookErrors.empty()) {
                cookErrors += "\n";
            }
            cookErrors += diagnostic.ToString();
        }
        ASSERT_TRUE(cook.Succeeded()) << cookErrors;
        // 只有默认变体, 故 1 VS + 1 PS。
        ASSERT_EQ(cook.Index.Entries.size(), 2u);
    }

    // ---- 阶段 3a: manifest -> PipelineLayout ----
    // 先做这步而不是先建 RT, 是因为它完全不碰 GPU: manifest 是唯一 ABI 来源,
    // 建 layout 不需要反射、不需要字节码、不需要变体。这条不变量正是 shader_asset
    // 的核心设计, 这里顺带验证它。
    ShaderAssetDiagnostic diag;
    std::optional<ShaderAssetDesc> asset = LoadShaderAssetDesc(manifestPath, diag);
    ASSERT_TRUE(asset.has_value()) << diag.ToString();
    ASSERT_EQ(asset->Passes.size(), 1u);
    const ShaderPassDesc& pass = asset->Passes.front();

    // manifest 允许 pass.Source 留空表示沿用资产级 Source, 而 ShaderResolver 只收
    // pass, 要求路径已展开。
    const ShaderPassDesc resolvablePass = MakeResolvablePass(asset.value(), pass);

    ShaderPipelineLayoutStorage layoutStorage = BuildPipelineLayoutStorage(pass);
    ASSERT_EQ(layoutStorage.GroupCount(), 2u) << "gPerObject group 0 + gView group 1";
    auto pipelineLayoutResult = device.CreatePipelineLayout(layoutStorage.Get());
    ASSERT_TRUE(pipelineLayoutResult.HasValue()) << "CreatePipelineLayout failed";
    unique_ptr<render::PipelineLayout> pipelineLayout = pipelineLayoutResult.Release();

    // ---- 阶段 3b: 变体 -> 字节码 -> Shader 对象 ----
    std::optional<ShaderVariantDomain> domain =
        ShaderVariantDomain::Build(asset.value(), pass, diag);
    ASSERT_TRUE(domain.has_value()) << diag.ToString();
    const ShaderVariantKey variant = domain->DefaultVariant();

    // JIT 模式: 无产物, resolver 现场编译 —— 验证 "AOT 未命中 -> JIT" 在真实 device
    // 前可用。
    // AOT 模式: dxc 传 nullptr 而非 dxc.get()。给了指针再设 AllowJit = false 只测到
    // "我们没去用它"; 传 nullptr 才测到"发布包里 DXC 根本不存在时也能起来"。
    const bool isAot = mode == SliceBytecodeMode::Aot;
    ShaderResolver resolver{
        ShaderResolveConfig{
            .ShaderRoot = shaderRoot,
            .ManifestPath = manifestPath,
            .Staleness = isAot ? ShaderArtifactStaleness::Lenient
                               : ShaderArtifactStaleness::Strict,
            .AllowJit = !isAot},
        isAot ? nullptr : dxc.get()};
    EXPECT_EQ(resolver.CanJit(), !isAot);

    const ShaderBytecodeSource expectedSource =
        isAot ? ShaderBytecodeSource::Artifact : ShaderBytecodeSource::Jit;

    const std::array<render::ShaderStage, 2> stages{
        render::ShaderStage::Vertex,
        render::ShaderStage::Pixel};

    // Shader 对象与字节码都要活到建完 PSO。
    vector<unique_ptr<render::Shader>> shaders;
    std::optional<render::ShaderEntry> vsEntry;
    std::optional<render::ShaderEntry> psEntry;
    for (render::ShaderStage stage : stages) {
        const vector<string> defines = domain->CollectDefines(variant, stage);
        std::optional<ShaderBytecode> bytecode =
            resolver.Resolve(resolvablePass, stage, category, defines, diag);
        ASSERT_TRUE(bytecode.has_value()) << "stage resolve failed: " << diag.ToString();
        EXPECT_FALSE(bytecode->Data.empty());
        // 断言来源: AOT 用例若因任何原因悄悄退回 JIT, 最终像素照样是洋红, 整个
        // AOT 参数化就白跑了。
        EXPECT_EQ(bytecode->Source, expectedSource);

        auto shaderResult = device.CreateShader(bytecode->MakeDescriptor());
        ASSERT_TRUE(shaderResult.HasValue()) << "CreateShader failed";
        shaders.push_back(shaderResult.Release());

        std::optional<std::string_view> entryPoint = pass.FindEntryPoint(stage);
        ASSERT_TRUE(entryPoint.has_value());
        render::ShaderEntry entry{shaders.back().get(), entryPoint.value()};
        if (stage == render::ShaderStage::Vertex) {
            vsEntry = entry;
        } else {
            psEntry = entry;
        }
    }
    ASSERT_TRUE(vsEntry.has_value());
    ASSERT_TRUE(psEntry.has_value());

    // ---- 阶段 2: 离屏 RT + RenderPass + Framebuffer ----
    // 必须在建 PSO 之前 —— Vulkan 要求 GraphicsPipelineStateDescriptor 显式给出
    // CompatibleRenderPass。
    render::TextureDescriptor rtDesc{
        .Dim = render::TextureDimension::Dim2D,
        .Width = kTargetWidth,
        .Height = kTargetHeight,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = kTargetFormat,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::RenderTarget | render::TextureUse::CopySource,
        .Hints = render::ResourceHint::None};
    auto rtResult = device.CreateTexture(rtDesc);
    ASSERT_TRUE(rtResult.HasValue()) << "CreateTexture failed";
    unique_ptr<render::Texture> renderTarget = rtResult.Release();

    render::TextureViewDescriptor rtvDesc{
        .Target = renderTarget.get(),
        .Dim = render::TextureDimension::Dim2D,
        .Format = kTargetFormat,
        .Range = render::SubresourceRange{0, 1, 0, 1},
        .Usage = render::TextureViewUsage::RenderTarget};
    auto rtvResult = device.CreateTextureView(rtvDesc);
    ASSERT_TRUE(rtvResult.HasValue()) << "CreateTextureView failed";
    unique_ptr<render::TextureView> rtv = rtvResult.Release();

    // Clear 成不透明黑, 与洋红有明显区别 —— 若最终读到黑色, 说明绘制没生效而非
    // 断言写错。
    const std::array<render::RenderPassColorAttachmentDescriptor, 1> colorAttachments{
        render::RenderPassColorAttachmentDescriptor{
            .Format = kTargetFormat,
            .SampleCount = 1,
            .Load = render::LoadAction::Clear,
            .Store = render::StoreAction::Store}};
    render::RenderPassDescriptor renderPassDesc{
        .ColorAttachments = colorAttachments,
        .DepthStencilAttachment = std::nullopt};
    auto renderPassResult = device.CreateRenderPass(renderPassDesc);
    ASSERT_TRUE(renderPassResult.HasValue()) << "CreateRenderPass failed";
    unique_ptr<render::RenderPass> renderPass = renderPassResult.Release();

    render::TextureView* const colorViews[]{rtv.get()};
    render::FramebufferDescriptor framebufferDesc{
        .Pass = renderPass.get(),
        .ColorAttachments = colorViews,
        .DepthStencilAttachment = nullptr,
        .Width = kTargetWidth,
        .Height = kTargetHeight,
        .Layers = 1};
    auto framebufferResult = device.CreateFramebuffer(framebufferDesc);
    ASSERT_TRUE(framebufferResult.HasValue()) << "CreateFramebuffer failed";
    unique_ptr<render::Framebuffer> framebuffer = framebufferResult.Release();

    // ---- 阶段 4: PSO ----
    ASSERT_TRUE(pass.VertexInput.has_value());
    ShaderVertexInputStorage vertexInputStorage =
        BuildVertexInputStorage(pass.VertexInput.value());

    const std::array<render::ColorTargetState, 1> colorTargets{
        render::ColorTargetState::Default(kTargetFormat)};

    render::PrimitiveState primitive = render::PrimitiveState::Default();
    // Default() 打开了 UnclippedDepth。三角形的 z 全是 0, 落在 [0,1] 内, 关掉它
    // 使切片走常规裁剪路径。
    primitive.UnclippedDepth = false;
    // 关剔除。Default() 是 FrontFace::CW + CullMode::Back, 而 D3D12 与 Vulkan 的 NDC
    // y 轴方向相反 —— 同一份顶点数据在两个后端的绕序正好相反, 实测 Vulkan 通过、
    // D3D12 整个三角形被剔掉。绕序约定属于"CPU 侧投影矩阵与网格数据的契约", 是
    // 材质/网格层要解决的问题, 本切片验证的是 shader 链路, 不该被它绊住。
    primitive.Cull = render::CullMode::None;

    render::GraphicsPipelineStateDescriptor psoDesc{
        .PipelineLayout = pipelineLayout.get(),
        .VS = vsEntry,
        .PS = psEntry,
        .VertexInput = vertexInputStorage.Get(),
        .Primitive = primitive,
        .DepthStencil = std::nullopt,
        .MultiSample = render::MultiSampleState::Default(),
        .ColorTargets = colorTargets,
        .CompatibleRenderPass = renderPass.get()};
    auto psoResult = device.CreateGraphicsPipelineState(psoDesc);
    ASSERT_TRUE(psoResult.HasValue()) << "CreateGraphicsPipelineState failed";
    unique_ptr<render::GraphicsPipelineState> pso = psoResult.Release();

    // ---- 阶段 5: 顶点/索引/常量缓冲 + 参数集 ----
    auto vertexBuffer = MakeUploadBuffer(
        device,
        std::as_bytes(std::span{kTriangleVertices}),
        render::BufferUse::Vertex);
    ASSERT_TRUE(vertexBuffer.HasValue());
    auto indexBuffer = MakeUploadBuffer(
        device,
        std::as_bytes(std::span{kTriangleIndices}),
        render::BufferUse::Index);
    ASSERT_TRUE(indexBuffer.HasValue());

    const ObjectConstantsCpu objectConstants{MakeIdentity()};
    auto objectBuffer = MakeUploadBuffer(
        device,
        std::as_bytes(std::span{&objectConstants, 1}),
        render::BufferUse::CBuffer);
    ASSERT_TRUE(objectBuffer.HasValue());

    // 光源数与阴影开关全留 0 —— error_pass 的 PS 不读 gView, VS 只读 ViewProj。
    ViewConstantsCpu viewConstants{};
    viewConstants.ViewProj = MakeIdentity();
    auto viewBuffer = MakeUploadBuffer(
        device,
        std::as_bytes(std::span{&viewConstants, 1}),
        render::BufferUse::CBuffer);
    ASSERT_TRUE(viewBuffer.HasValue());

    // 每个 binding group 一个参数集。group 索引来自 manifest, 不是硬编码。
    vector<unique_ptr<render::ShaderParameterSet>> parameterSets;
    vector<uint32_t> parameterGroups;
    for (const ShaderBindingGroupDesc& group : pass.BindingGroups) {
        render::ShaderParameterSetDescriptor setDesc{
            .Layout = pipelineLayout.get(),
            .GroupIndex = group.Group};
        auto setResult = device.CreateShaderParameterSet(setDesc);
        ASSERT_TRUE(setResult.HasValue()) << "CreateShaderParameterSet failed for group "
                                         << group.Group;
        unique_ptr<render::ShaderParameterSet> set = setResult.Release();

        for (const ShaderBindingDesc& binding : group.Bindings) {
            render::Buffer* target = nullptr;
            uint64_t size = 0;
            if (binding.Name == "gPerObject") {
                target = objectBuffer.Get();
                size = sizeof(ObjectConstantsCpu);
            } else if (binding.Name == "gView") {
                target = viewBuffer.Get();
                size = sizeof(ViewConstantsCpu);
            }
            ASSERT_NE(target, nullptr) << "unexpected binding " << binding.Name;
            ASSERT_TRUE(set->Set(
                binding.Binding,
                0,
                render::ShaderParameterValue{render::ShaderBufferBinding{
                    .Target = target,
                    .Range = render::BufferRange{0, size},
                    .StructureByteStride = 0}}))
                << "Set failed for " << binding.Name;
        }
        ASSERT_TRUE(set->FlushWrites()) << "FlushWrites failed for group " << group.Group;
        parameterSets.push_back(std::move(set));
        parameterGroups.push_back(group.Group);
    }
    ASSERT_EQ(parameterSets.size(), 2u);

    // ---- 阶段 7 准备: readback buffer ----
    // row pitch 由后端按 TextureDataPitchAlignment 对齐, 读的时候必须按 pitch 走行,
    // 不能按 width * bpp。
    const render::DeviceDetail detail = device.GetDetail();
    const uint32_t bytesPerPixel = render::GetTextureFormatBytesPerPixel(kTargetFormat);
    ASSERT_EQ(bytesPerPixel, 4u);
    const uint64_t rowPitch =
        Align(static_cast<uint64_t>(kTargetWidth) * bytesPerPixel,
              detail.TextureDataPitchAlignment);
    const uint64_t readbackSize = rowPitch * kTargetHeight;

    render::BufferDescriptor readbackDesc{
        .Size = readbackSize,
        .Memory = render::MemoryType::ReadBack,
        .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead,
        .Hints = render::ResourceHint::None};
    auto readbackResult = device.CreateBuffer(readbackDesc);
    ASSERT_TRUE(readbackResult.HasValue()) << "CreateBuffer (readback) failed";
    unique_ptr<render::Buffer> readback = readbackResult.Release();

    // ---- 阶段 6: 录制并提交 ----
    auto cmdResult = device.CreateCommandBuffer(ctx.Queue);
    ASSERT_TRUE(cmdResult.HasValue()) << "CreateCommandBuffer failed";
    unique_ptr<render::CommandBuffer> cmd = cmdResult.Release();

    cmd->Begin();
    {
        // 新建纹理的初始状态: Vulkan 是 UNDEFINED, D3D12 是 COMMON。给 Undefined
        // 两边都安全 (D3D12 在 Before == After 时会跳过 barrier)。
        render::ResourceBarrierDescriptor toRenderTarget = render::BarrierTextureDescriptor{
            .Target = renderTarget.get(),
            .Before = render::TextureState::Undefined,
            .After = render::TextureState::RenderTarget};
        cmd->ResourceBarrier(std::span{&toRenderTarget, 1});
    }

    const std::array<render::ColorClearValue, 1> clearValues{
        render::ColorClearValue{{0.0f, 0.0f, 0.0f, 1.0f}}};
    render::RenderPassBeginDescriptor beginDesc{
        .Pass = renderPass.get(),
        .Target = framebuffer.get(),
        .ColorClearValues = clearValues,
        .DepthStencilClearValue = std::nullopt,
        .Name = "vertical_slice"};
    auto encoderResult = cmd->BeginRenderPass(beginDesc);
    ASSERT_TRUE(encoderResult.HasValue()) << "BeginRenderPass failed";
    unique_ptr<render::GraphicsCommandEncoder> encoder = encoderResult.Release();

    // Viewport / Rect 在 namespace radray, 不是 radray::render。
    encoder->SetViewport(Viewport{
        0.0f, 0.0f,
        static_cast<float>(kTargetWidth), static_cast<float>(kTargetHeight),
        0.0f, 1.0f});
    encoder->SetScissor(Rect{0, 0, kTargetWidth, kTargetHeight});

    // 必须先绑 PSO 再绑参数集 —— Vulkan 侧 BindShaderParameterSet 依赖 PSO 绑定时
    // 记下的 layout。
    encoder->BindGraphicsPipelineState(pso.get());
    for (size_t i = 0; i < parameterSets.size(); ++i) {
        encoder->BindShaderParameterSet(parameterGroups[i], parameterSets[i].get());
    }

    const std::array<render::VertexBufferBinding, 1> vertexBindings{
        render::VertexBufferBinding{
            .Binding = 0,
            .View = render::VertexBufferView{
                .Target = vertexBuffer.Get(),
                .Offset = 0,
                .Size = kTriangleVertices.size() * sizeof(float)}}};
    encoder->BindVertexBuffers(vertexBindings);
    encoder->BindIndexBuffer(render::IndexBufferView{
        .Target = indexBuffer.Get(),
        .Offset = 0,
        .Stride = sizeof(uint16_t)});
    encoder->DrawIndexed(
        static_cast<uint32_t>(kTriangleIndices.size()), 1, 0, 0, 0);

    cmd->EndRenderPass(std::move(encoder));

    {
        render::ResourceBarrierDescriptor toCopySource = render::BarrierTextureDescriptor{
            .Target = renderTarget.get(),
            .Before = render::TextureState::RenderTarget,
            .After = render::TextureState::CopySource};
        cmd->ResourceBarrier(std::span{&toCopySource, 1});
    }
    // MipLevelCount / ArrayLayerCount 不能用 SubresourceRange::All, 两个后端都会拒绝。
    cmd->CopyTextureToBuffer(
        readback.get(), 0, renderTarget.get(), render::SubresourceRange{0, 1, 0, 1});
    cmd->End();

    render::CommandBuffer* submitBuffers[]{cmd.get()};
    ctx.Queue->Submit(render::CommandQueueSubmitDescriptor{.CmdBuffers = submitBuffers});
    ctx.Queue->Wait();

    // ---- 阶段 7: 断言像素 ----
    void* mapped = readback->Map(0, readbackSize);
    ASSERT_NE(mapped, nullptr) << "Map (readback) failed";
    readback->InvalidateMappedRange(render::BufferRange{0, readbackSize});

    const auto* base = static_cast<const uint8_t*>(mapped);
    const auto readPixel = [&](uint32_t x, uint32_t y) -> std::array<uint8_t, 4> {
        const uint8_t* row = base + static_cast<uint64_t>(y) * rowPitch;
        const uint8_t* texel = row + static_cast<uint64_t>(x) * bytesPerPixel;
        return {texel[0], texel[1], texel[2], texel[3]};
    };

    // 中心必定被三角形覆盖 -> PSMain 的洋红。
    const std::array<uint8_t, 4> center = readPixel(kTargetWidth / 2, kTargetHeight / 2);
    EXPECT_EQ(center[0], 255) << "center R";
    EXPECT_EQ(center[1], 0) << "center G";
    EXPECT_EQ(center[2], 255) << "center B";
    EXPECT_EQ(center[3], 255) << "center A";

    // 左上角在三角形外 -> clear 的黑色。这条断言保证上面读到的洋红来自光栅化,
    // 而不是整张图都被填成了同一个颜色。
    const std::array<uint8_t, 4> corner = readPixel(0, 0);
    EXPECT_EQ(corner[0], 0) << "corner R";
    EXPECT_EQ(corner[1], 0) << "corner G";
    EXPECT_EQ(corner[2], 0) << "corner B";

    readback->Unmap();
}

/// 参数集在宏外面算好。宏参数列表里既不能出现裸逗号, 也不能塞 `#if`
/// (MSVC 的 /Zc:preprocessor 会把它判成语法错误)。
vector<SliceParams> MakeSliceParamList() {
    vector<SliceParams> params;
    const std::array<render::RenderBackend, 2> backends{
        render::RenderBackend::D3D12,
        render::RenderBackend::Vulkan};
    for (const render::RenderBackend backend : backends) {
#if !defined(RADRAY_ENABLE_D3D12)
        if (backend == render::RenderBackend::D3D12) {
            continue;
        }
#endif
        params.push_back(MakeSliceParams(backend, SliceBytecodeMode::Jit));
        params.push_back(MakeSliceParams(backend, SliceBytecodeMode::Aot));
    }
    return params;
}

INSTANTIATE_TEST_SUITE_P(
    Backends,
    VerticalSliceTest,
    testing::ValuesIn(MakeSliceParamList()),
    [](const testing::TestParamInfo<SliceParams>& info) {
        const char* backend =
            info.param.Backend == render::RenderBackend::D3D12 ? "D3D12" : "Vulkan";
        const char* mode = info.param.Mode == SliceBytecodeMode::Aot ? "Aot" : "Jit";
        return string{backend} + "_" + mode;
    });

}  // namespace
}  // namespace radray
