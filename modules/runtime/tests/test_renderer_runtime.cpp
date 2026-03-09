#include <array>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include <radray/triangle_mesh.h>
#include <radray/runtime/frame_snapshot_builder.h>
#include <radray/runtime/renderer_runtime.h>
#include <radray/window/native_window.h>

#include "runtime_test_framework.h"

using namespace radray;
using namespace radray::runtime;

namespace {

constexpr uint32_t kInitialWidth = 640;
constexpr uint32_t kInitialHeight = 360;
constexpr uint32_t kResizedWidth = 800;
constexpr uint32_t kResizedHeight = 450;

Nullable<unique_ptr<NativeWindow>> CreateTestWindow(uint32_t width, uint32_t height) noexcept {
#if defined(_WIN32)
    Win32WindowCreateDescriptor desc{};
    desc.Title = "RendererRuntimeSmoke";
    desc.Width = static_cast<int32_t>(width);
    desc.Height = static_cast<int32_t>(height);
    desc.X = 120;
    desc.Y = 120;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return CreateNativeWindow(desc);
#elif defined(__APPLE__)
    CocoaWindowCreateDescriptor desc{};
    desc.Title = "RendererRuntimeSmoke";
    desc.Width = static_cast<int32_t>(width);
    desc.Height = static_cast<int32_t>(height);
    desc.X = 120;
    desc.Y = 120;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return CreateNativeWindow(desc);
#else
    RADRAY_UNUSED(width);
    RADRAY_UNUSED(height);
    return nullptr;
#endif
}

render::SamplerDescriptor CreateLinearClampSamplerDesc() {
    render::SamplerDescriptor desc{};
    desc.AddressS = render::AddressMode::ClampToEdge;
    desc.AddressT = render::AddressMode::ClampToEdge;
    desc.AddressR = render::AddressMode::ClampToEdge;
    desc.MinFilter = render::FilterMode::Linear;
    desc.MagFilter = render::FilterMode::Linear;
    desc.MipmapFilter = render::FilterMode::Linear;
    desc.LodMin = 0.0f;
    desc.LodMax = 0.0f;
    desc.AnisotropyClamp = 1;
    return desc;
}

vector<byte> CreateSolidTextureBytes(uint32_t width, uint32_t height, std::array<uint8_t, 4> rgba) {
    vector<byte> texels(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < texels.size(); i += 4) {
        texels[i + 0] = static_cast<byte>(rgba[0]);
        texels[i + 1] = static_cast<byte>(rgba[1]);
        texels[i + 2] = static_cast<byte>(rgba[2]);
        texels[i + 3] = static_cast<byte>(rgba[3]);
    }
    return texels;
}

FrameSnapshot BuildQuadSnapshot(uint32_t width, uint32_t height, MeshHandle mesh, MaterialHandle material) {
    FrameSnapshotBuilder builder{};
    builder.Reset(1, 1);
    builder.AddView() = RenderViewRequest{
        .ViewId = 0,
        .Type = RenderViewType::MainColor,
        .CameraId = 1,
        .OutputWidth = width,
        .OutputHeight = height,
    };
    auto& camera = builder.AddCamera();
    camera.CameraId = 1;
    camera.ViewId = 0;
    camera.View = Eigen::Matrix4f::Identity();
    camera.Proj = Eigen::Matrix4f::Identity();
    camera.ViewProj = Eigen::Matrix4f::Identity();
    camera.InvView = Eigen::Matrix4f::Identity();
    camera.InvProj = Eigen::Matrix4f::Identity();
    camera.OutputWidth = width;
    camera.OutputHeight = height;
    builder.AddMeshBatch() = VisibleMeshBatch{
        .InstanceId = 1,
        .ViewMask = 0,
        .Mesh = mesh,
        .Material = material,
        .SubmeshIndex = 0,
        .LocalToWorld = Eigen::Matrix4f::Identity(),
        .PrevLocalToWorld = Eigen::Matrix4f::Identity(),
    };

    string reason{};
    FrameSnapshot snapshot = builder.Finalize(&reason);
    EXPECT_TRUE(reason.empty()) << reason;
    return snapshot;
}

MeshResource CreateQuadMeshResource() {
    TriangleMesh mesh{};
    mesh.InitAsRectXY(2.0f, 2.0f);
    MeshResource resource{};
    mesh.ToSimpleMeshResource(&resource);
    resource.Name = "runtime_smoke_quad";
    return resource;
}

void DispatchWindowFrames(NativeWindow* window, uint32_t iterations) {
    if (window == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < iterations; ++i) {
        window->DispatchEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void RunRuntimeScenario(runtime::test::TestBackend backend) {
    auto windowOpt = CreateTestWindow(kInitialWidth, kInitialHeight);
    if (!windowOpt.HasValue()) {
        GTEST_SKIP() << "Cannot create native window for this platform.";
    }
    auto window = windowOpt.Release();

    runtime::test::RuntimeTestContext ctx{};
    string reason{};
    if (!ctx.Initialize(backend, &reason)) {
        GTEST_SKIP() << reason;
    }

    const auto initialSize = window->GetSize();
    if (initialSize.X <= 0 || initialSize.Y <= 0) {
        GTEST_SKIP() << "Window size is invalid before runtime initialization.";
    }

    RendererRuntime renderer{};
    RendererRuntimeCreateDesc createDesc{};
    createDesc.Device = ctx.GetDevice();
    createDesc.NativeHandler = window->GetNativeHandler().Handle;
    createDesc.Width = static_cast<uint32_t>(initialSize.X);
    createDesc.Height = static_cast<uint32_t>(initialSize.Y);
    createDesc.BackBufferCount = 3;
    createDesc.FlightFrameCount = 2;
    createDesc.Format = render::TextureFormat::BGRA8_UNORM;
    createDesc.PresentMode = render::PresentMode::FIFO;
    ASSERT_TRUE(renderer.Initialize(createDesc, &reason)) << reason;

    RenderAssetRegistry& assets = renderer.Assets();
    const MeshHandle meshHandle = assets.RegisterMesh(CreateQuadMeshResource());
    ASSERT_TRUE(meshHandle.IsValid());

    TextureInitDesc textureDesc{};
    textureDesc.DebugName = "runtime_smoke_texture";
    textureDesc.Desc.Dim = render::TextureDimension::Dim2D;
    textureDesc.Desc.Width = 2;
    textureDesc.Desc.Height = 2;
    textureDesc.Desc.DepthOrArraySize = 1;
    textureDesc.Desc.MipLevels = 1;
    textureDesc.Desc.SampleCount = 1;
    textureDesc.Desc.Format = render::TextureFormat::RGBA8_UNORM;
    textureDesc.Desc.Memory = render::MemoryType::Device;
    textureDesc.Desc.Usage = render::TextureUse::Resource;
    const TextureHandle textureHandle = assets.RegisterTexture2D(
        textureDesc,
        CreateSolidTextureBytes(2, 2, {32, 220, 96, 255}));
    ASSERT_TRUE(textureHandle.IsValid());

    const SamplerHandle samplerHandle = assets.RegisterSampler(CreateLinearClampSamplerDesc());
    ASSERT_TRUE(samplerHandle.IsValid());

    const MaterialHandle materialHandle = assets.RegisterForwardOpaqueMaterial(ForwardOpaqueMaterialDesc{
        .Albedo = textureHandle,
        .Sampler = samplerHandle,
        .Tint = Eigen::Vector4f{1.0f, 1.0f, 1.0f, 1.0f},
        .DebugName = "runtime_smoke_material",
    });
    ASSERT_TRUE(materialHandle.IsValid());
    ASSERT_EQ(assets.GetPendingUploads().size(), 2u);

    renderer.SetCaptureEnabled(true);
    window->DispatchEvents();
    ASSERT_TRUE(renderer.RenderFrame(
        BuildQuadSnapshot(createDesc.Width, createDesc.Height, meshHandle, materialHandle),
        &reason))
        << reason;
    renderer.WaitIdle();
    ASSERT_TRUE(assets.GetPendingUploads().empty());

    auto pixel = renderer.ReadCapturedPixel(createDesc.Width / 2, createDesc.Height / 2, &reason);
    ASSERT_TRUE(pixel.has_value()) << reason;
    EXPECT_NE(pixel.value(), 0xFF000000u);
    EXPECT_NE(pixel.value() & 0x00FFFFFFu, 0u);

    window->SetSize(static_cast<int>(kResizedWidth), static_cast<int>(kResizedHeight));
    DispatchWindowFrames(window.get(), 32);
    auto resizedSize = window->GetSize();
    if (resizedSize.X <= 0 || resizedSize.Y <= 0) {
        resizedSize = WindowVec2i{
            static_cast<int32_t>(kResizedWidth),
            static_cast<int32_t>(kResizedHeight),
        };
    }
    renderer.Resize(static_cast<uint32_t>(resizedSize.X), static_cast<uint32_t>(resizedSize.Y));
    window->DispatchEvents();
    ASSERT_TRUE(renderer.RenderFrame(
        BuildQuadSnapshot(
            static_cast<uint32_t>(resizedSize.X),
            static_cast<uint32_t>(resizedSize.Y),
            meshHandle,
            materialHandle),
        &reason))
        << reason;
    renderer.WaitIdle();

    pixel = renderer.ReadCapturedPixel(
        static_cast<uint32_t>(resizedSize.X / 2),
        static_cast<uint32_t>(resizedSize.Y / 2),
        &reason);
    ASSERT_TRUE(pixel.has_value()) << reason;
    EXPECT_NE(pixel.value(), 0xFF000000u);

    const auto errors = ctx.GetCapturedErrors();
    EXPECT_TRUE(errors.empty()) << ctx.JoinCapturedErrors();

    renderer.Destroy();
    window->Destroy();
}

}  // namespace

TEST(RendererRuntimeTest, SmokeAndResize_D3D12) {
    RunRuntimeScenario(runtime::test::TestBackend::D3D12);
}

TEST(RendererRuntimeTest, SmokeAndResize_Vulkan) {
    RunRuntimeScenario(runtime::test::TestBackend::Vulkan);
}
