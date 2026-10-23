#include "gpu_test_fixture.h"

#if defined(RADRAY_ENABLE_VULKAN)

namespace radray::render::vulkan {
namespace {

struct RenderObjectProbe {
    uint32_t PassCreates{0}, FramebufferCreates{0}, FramebufferDestroys{0}, Begins{0};
    bool FailPass{false}, FailFramebuffer{false};
    uintptr_t NextHandle{100};
    vector<VkAttachmentDescription2> PassAttachments;
    vector<VkAttachmentReference2> ColorReferences;
    std::optional<VkAttachmentReference2> DepthReference;
    vector<FramebufferAttachmentVulkan> Constraints;
    vector<VkFormat> Formats;
    vector<VkImageView> BeginViews;
    VkFramebuffer LastFramebuffer{VK_NULL_HANDLE};
    VkRect2D LastArea{};
    InstanceVulkanImpl Instance{VK_NULL_HANDLE, std::nullopt, {}, {}};
    DeviceVulkan Device{&Instance, VK_NULL_HANDLE, reinterpret_cast<VkDevice>(this)};
    QueueVulkan Queue{&Device, VK_NULL_HANDLE, {}, QueueType::Direct, 0};
    CommandAllocatorVulkan Allocator{&Device, &Queue, VK_NULL_HANDLE};
    CommandBufferVulkan Command{&Device, &Queue, &Allocator, reinterpret_cast<VkCommandBuffer>(this)};
    vector<test::RenderTarget> Targets;

    template <class T>
    T Handle() { return reinterpret_cast<T>(NextHandle++); }
    static RenderObjectProbe& From(VkDevice device) { return *reinterpret_cast<RenderObjectProbe*>(device); }

    explicit RenderObjectProbe(FramebufferModeVulkan mode) {
        Device._ftb = {};
        Device._renderPassCache = make_unique<VulkanRenderPassCache>(&Device, mode);
        Device._ftb.vkDestroyDevice = [](VkDevice, const VkAllocationCallbacks*) {};
        Device._ftb.vkCreateRenderPass2 = [](VkDevice device, const VkRenderPassCreateInfo2* info, const VkAllocationCallbacks*, VkRenderPass* pass) {
            auto& p = From(device);
            ++p.PassCreates;
            EXPECT_EQ(info->sType, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2);
            EXPECT_EQ(info->subpassCount, 1u);
            p.PassAttachments.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
            const auto& subpass = info->pSubpasses[0];
            EXPECT_EQ(subpass.sType, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2);
            EXPECT_EQ(subpass.viewMask, 0u);
            p.ColorReferences.clear();
            for (uint32_t i = 0; i < subpass.colorAttachmentCount; ++i) p.ColorReferences.push_back(subpass.pColorAttachments[i]);
            p.DepthReference = subpass.pDepthStencilAttachment ? std::optional{*subpass.pDepthStencilAttachment} : std::nullopt;
            if (p.FailPass) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *pass = p.Handle<VkRenderPass>();
            return VK_SUCCESS;
        };
        Device._ftb.vkDestroyRenderPass = [](VkDevice, VkRenderPass, const VkAllocationCallbacks*) {};
        Device._ftb.vkCreateFramebuffer = [](VkDevice device, const VkFramebufferCreateInfo* info, const VkAllocationCallbacks*, VkFramebuffer* framebuffer) {
            auto& p = From(device);
            ++p.FramebufferCreates;
            p.Constraints.clear();
            p.Formats.clear();
            if (info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) {
                EXPECT_EQ(info->pAttachments, nullptr);
                const auto* attachments = static_cast<const VkFramebufferAttachmentsCreateInfo*>(info->pNext);
                EXPECT_NE(attachments, nullptr);
                if (!attachments) return VK_ERROR_INITIALIZATION_FAILED;
                EXPECT_EQ(attachments->sType, VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO);
                EXPECT_EQ(attachments->attachmentImageInfoCount, info->attachmentCount);
                for (uint32_t i = 0; i < attachments->attachmentImageInfoCount; ++i) {
                    const auto& source = attachments->pAttachmentImageInfos[i];
                    p.Constraints.push_back({source.flags, source.usage, source.width, source.height, source.layerCount});
                    EXPECT_EQ(source.viewFormatCount, 1u);
                    p.Formats.push_back(source.pViewFormats[0]);
                }
            } else {
                EXPECT_EQ(info->pNext, nullptr);
                EXPECT_NE(info->pAttachments, nullptr);
            }
            if (p.FailFramebuffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            *framebuffer = p.Handle<VkFramebuffer>();
            return VK_SUCCESS;
        };
        Device._ftb.vkDestroyFramebuffer = [](VkDevice device, VkFramebuffer, const VkAllocationCallbacks*) { ++From(device).FramebufferDestroys; };
        Device._ftb.vkDestroyImageView = [](VkDevice, VkImageView, const VkAllocationCallbacks*) {};
        Device._ftb.vkBeginCommandBuffer = [](VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; };
        Device._ftb.vkEndCommandBuffer = [](VkCommandBuffer) { return VK_SUCCESS; };
        Device._ftb.vkResetCommandPool = [](VkDevice, VkCommandPool, VkCommandPoolResetFlags) { return VK_SUCCESS; };
        Device._ftb.vkFreeCommandBuffers = [](VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*) {};
        Device._ftb.vkCmdBeginRenderPass2 = [](VkCommandBuffer command, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* begin) {
            auto& p = *reinterpret_cast<RenderObjectProbe*>(command);
            ++p.Begins;
            p.LastFramebuffer = info->framebuffer;
            p.LastArea = info->renderArea;
            EXPECT_EQ(begin->sType, VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO);
            EXPECT_EQ(begin->contents, VK_SUBPASS_CONTENTS_INLINE);
            p.BeginViews.clear();
            if (info->pNext) {
                const auto* attachments = static_cast<const VkRenderPassAttachmentBeginInfo*>(info->pNext);
                EXPECT_EQ(attachments->sType, VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO);
                p.BeginViews.assign(attachments->pAttachments, attachments->pAttachments + attachments->attachmentCount);
            }
        };
        Device._ftb.vkCmdEndRenderPass2 = [](VkCommandBuffer, const VkSubpassEndInfo* end) {
            EXPECT_EQ(end->sType, VK_STRUCTURE_TYPE_SUBPASS_END_INFO);
        };
        Command.Begin();
    }

    ~RenderObjectProbe() {
        Command.Destroy();
        Targets.clear();
    }

    ImageViewVulkan* AddTarget(TextureFormat format = TextureFormat::RGBA8_UNORM, uint32_t mip = 0, uint32_t layers = 1) {
        auto image = make_unique<ImageVulkan>(&Device, Handle<VkImage>(), VK_NULL_HANDLE, VmaAllocationInfo{});
        image->_hints = ResourceHint::External;
        image->_dim = TextureDimension::Dim2DArray;
        image->_format = format;
        image->_rawFormat = MapType(format);
        image->_width = 32;
        image->_height = 32;
        image->_depthOrArraySize = layers;
        image->_mipLevels = 4;
        image->_sampleCount = 1;
        image->_createFlags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        image->_nativeUsage = IsDepthStencilFormat(format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        auto view = make_unique<ImageViewVulkan>(&Device, image.get(), Handle<VkImageView>());
        view->_mdesc = {image.get(), TextureDimension::Dim2DArray, format, {0, layers, mip, 1}, IsDepthStencilFormat(format) ? TextureViewUsage::DepthWrite : TextureViewUsage::RenderTarget};
        view->_rawFormat = MapType(format);
        view->_nativeUsage = image->_nativeUsage;
        view->_nativeRange = ResolveSubresourceRange(image->GetDesc(), view->_mdesc.Range);
        auto* result = view.get();
        Targets.push_back({std::move(image), std::move(view)});
        return result;
    }

    bool Render(ImageViewVulkan* view, uint32_t size = 8) {
        const RenderPassColorAttachment attachment{view, LoadAction::Clear, StoreAction::Store, {}};
        auto encoder = Command.BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = size, .Height = size});
        if (!encoder) return false;
        Command.EndRenderPass(encoder.Release());
        return true;
    }
};

TEST(VulkanRenderObjects, SameConstraintsDifferentViewsReuseOnlyImagelessFramebuffer) {
    for (const auto mode : {FramebufferModeVulkan::Traditional, FramebufferModeVulkan::Imageless}) {
        RenderObjectProbe p{mode};
        auto* a = p.AddTarget();
        auto* b = p.AddTarget();
        ASSERT_TRUE(p.Render(a));
        const auto first = p.LastFramebuffer;
        ASSERT_TRUE(p.Render(b));
        EXPECT_EQ(p.LastFramebuffer == first, mode == FramebufferModeVulkan::Imageless);
        EXPECT_EQ(p.FramebufferCreates, mode == FramebufferModeVulkan::Imageless ? 1u : 2u);
        EXPECT_EQ(p.PassCreates, 1u);
        if (mode == FramebufferModeVulkan::Imageless) EXPECT_EQ(p.BeginViews, (vector<VkImageView>{b->_imageView}));
        ASSERT_TRUE(p.Render(a));
        EXPECT_EQ(p.Command._attachmentViewPins.size(), 2u);
        p.Command.End();
        EXPECT_EQ(p.Command._attachmentViewPins.size(), 2u);
        EXPECT_DEATH(a->Destroy(), "");
        p.Allocator.Reset();
        EXPECT_TRUE(p.Command._attachmentViewPins.empty());
        a->Destroy();
        EXPECT_EQ(p.FramebufferDestroys, mode == FramebufferModeVulkan::Imageless ? 0u : 1u);
        p.Command.Begin();
        auto* c = p.AddTarget();
        ASSERT_TRUE(p.Render(c));
        EXPECT_EQ(p.FramebufferCreates, mode == FramebufferModeVulkan::Imageless ? 1u : 3u);
    }
}

TEST(VulkanRenderObjects, ImagelessKeySeparatesEveryNativeConstraint) {
    RenderObjectProbe p{FramebufferModeVulkan::Imageless};
    auto* base = p.AddTarget();
    ASSERT_TRUE(p.Render(base));
    auto* flags = p.AddTarget();
    flags->_image->_createFlags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ASSERT_TRUE(p.Render(flags));
    auto* usage = p.AddTarget();
    usage->_nativeUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    ASSERT_TRUE(p.Render(usage));
    auto* mip = p.AddTarget(TextureFormat::RGBA8_UNORM, 1);
    ASSERT_TRUE(p.Render(mip));
    EXPECT_EQ(p.Constraints[0].Width, 16u);
    auto* layers = p.AddTarget(TextureFormat::RGBA8_UNORM, 0, 2);
    ASSERT_TRUE(p.Render(layers));
    EXPECT_EQ(p.Constraints[0].Layers, 2u);
    auto* format = p.AddTarget(TextureFormat::RGBA8_UNORM_SRGB);
    ASSERT_TRUE(p.Render(format));
    EXPECT_EQ(p.Formats[0], VK_FORMAT_R8G8B8A8_SRGB);
    ASSERT_TRUE(p.Render(base, 4));
    EXPECT_EQ(p.FramebufferCreates, 7u);
}

TEST(VulkanRenderObjects, FailuresAndLiveBudgetDoNotBeginOrPinViews) {
    for (const auto mode : {FramebufferModeVulkan::Traditional, FramebufferModeVulkan::Imageless}) {
        RenderObjectProbe p{mode};
        auto* view = p.AddTarget();
        p.FailPass = true;
        EXPECT_FALSE(p.Render(view));
        EXPECT_TRUE(p.Command._attachmentViewPins.empty());
        p.FailPass = false;
        p.FailFramebuffer = true;
        EXPECT_FALSE(p.Render(view));
        EXPECT_EQ(p.Begins, 0u);
        EXPECT_TRUE(p.Command._attachmentViewPins.empty());
        p.FailFramebuffer = false;
        p.Device._renderPassCache->SetBudget({.Framebuffers = 1});
        ASSERT_TRUE(p.Render(view));
        auto* other = p.AddTarget(TextureFormat::RGBA8_UNORM_SRGB);
        EXPECT_FALSE(p.Render(other));
        EXPECT_EQ(p.Begins, 1u);
        EXPECT_EQ(p.Command._attachmentViewPins.size(), 1u);
        p.Allocator.Reset();
        p.Command.Begin();
        ASSERT_TRUE(p.Render(other));
        EXPECT_EQ(p.FramebufferDestroys, 1u);
    }
}

TEST(VulkanRenderObjects, MultipleCommandsIndependentlyRetainTheSameView) {
    RenderObjectProbe p{FramebufferModeVulkan::Imageless};
    auto* view = p.AddTarget();
    ASSERT_TRUE(p.Render(view));
    CommandBufferVulkan other{&p.Device, &p.Queue, &p.Allocator, reinterpret_cast<VkCommandBuffer>(&p)};
    other.Begin();
    const RenderPassColorAttachment attachment{view, LoadAction::Clear, StoreAction::Store, {}};
    auto encoder = other.BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = 8, .Height = 8}).Unwrap();
    other.EndRenderPass(std::move(encoder));
    p.Command.Destroy();
    EXPECT_DEATH(view->Destroy(), "");
    other.Destroy();
    view->Destroy();
}

TEST(VulkanRenderObjects, RenderPass2PreservesAttachmentOperationsAndLayouts) {
    RenderObjectProbe p{FramebufferModeVulkan::Imageless};
    auto* color = p.AddTarget();
    auto* depth = p.AddTarget(TextureFormat::D32_FLOAT_S8_UINT);
    const RenderPassColorAttachment colors[]{
        {color, LoadAction::Load, StoreAction::Store, {}},
        {p.AddTarget(TextureFormat::RGBA8_UINT), LoadAction::DontCare, StoreAction::Discard, {}}};
    auto encoder = p.Command.BeginRenderPass({.Colors = colors,
                                              .DepthStencil = RenderPassDepthStencilAttachment{depth, LoadAction::Load, StoreAction::Store, LoadAction::Load, StoreAction::Store, true, {}},
                                              .Width = 8,
                                              .Height = 8,
                                              .RenderArea = Rect{2, 3, 4, 5}})
                       .Unwrap();
    p.Command.EndRenderPass(std::move(encoder));
    ASSERT_EQ(p.PassAttachments.size(), 3u);
    for (const auto& attachment : p.PassAttachments) {
        EXPECT_EQ(attachment.sType, VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2);
        EXPECT_EQ(attachment.pNext, nullptr);
    }
    EXPECT_EQ(p.PassAttachments[0].loadOp, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_EQ(p.PassAttachments[1].storeOp, VK_ATTACHMENT_STORE_OP_DONT_CARE);
    EXPECT_EQ(p.PassAttachments[2].initialLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    ASSERT_TRUE(p.DepthReference);
    EXPECT_EQ(p.DepthReference->aspectMask, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    EXPECT_EQ(p.ColorReferences[0].aspectMask, VK_IMAGE_ASPECT_COLOR_BIT);
    EXPECT_EQ(p.LastArea.offset.x, 2);
    EXPECT_EQ(p.LastArea.extent.height, 5u);
}

TEST(VulkanViewFormats, CompatibilityClassesDoNotTreatDepthAsColor) {
    EXPECT_TRUE(AreViewFormatsCompatibleVulkan(TextureFormat::RGBA8_UNORM, TextureFormat::R32_UINT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT));
    EXPECT_FALSE(AreViewFormatsCompatibleVulkan(TextureFormat::RGBA8_UNORM, TextureFormat::RGBA8_UNORM_SRGB, 0));
    EXPECT_TRUE(AreViewFormatsCompatibleVulkan(TextureFormat::D32_FLOAT, TextureFormat::D32_FLOAT, 0));
    EXPECT_FALSE(AreViewFormatsCompatibleVulkan(TextureFormat::D32_FLOAT, TextureFormat::R32_FLOAT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT));
    EXPECT_FALSE(AreViewFormatsCompatibleVulkan(TextureFormat::D32_FLOAT_S8_UINT, TextureFormat::RG32_UINT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT));
}

class VulkanRenderScopeGpu : public testing::TestWithParam<FramebufferModeVulkan> {
protected:
    test::DeviceContext Context;
    void SetUp() override {
        if (!test::TryCreateDevice(RenderBackend::Vulkan, Context, true)) {
            if (test::SetupMustFail(Context.Status, test::RequiredBackend(RenderBackend::Vulkan))) FAIL() << Context.Reason;
            GTEST_SKIP() << Context.Reason;
        }
        auto& device = static_cast<DeviceVulkan&>(*Context.Device);
        if (GetParam() == FramebufferModeVulkan::Imageless && !device._enabledFeatures.Vulkan12.imagelessFramebuffer)
            GTEST_SKIP() << "imagelessFramebuffer is unavailable";
        device._renderPassCache = make_unique<VulkanRenderPassCache>(&device, GetParam());
    }
    void TearDown() override { Context.Reset(); }
};

TEST_P(VulkanRenderScopeGpu, MrtPartialAreaLoadStoreAndReuseReadBackIdentically) {
    auto& device = *Context.Device;
    auto a = test::MakeRenderTarget(&device, TextureFormat::RGBA8_UNORM, 8, 8, TextureUse::RenderTarget | TextureUse::CopySource);
    auto b = test::MakeRenderTarget(&device, TextureFormat::RGBA8_UNORM, 8, 8, TextureUse::RenderTarget | TextureUse::CopySource);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    const auto alignment = std::max<uint64_t>(1, device.GetCapabilities().Detail.TextureDataPitchAlignment);
    const auto rowPitch = (32 + alignment - 1) / alignment * alignment;
    auto readback = device.CreateBuffer({.Size = rowPitch * 8, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor barriers[]{
        BarrierTextureDescriptor{a->Tex.get(), TextureState::Undefined, TextureState::RenderTarget},
        BarrierTextureDescriptor{b->Tex.get(), TextureState::Undefined, TextureState::RenderTarget}};
    command->ResourceBarrier(barriers);
    RenderPassColorAttachment attachments[]{
        {a->View.get(), LoadAction::Clear, StoreAction::Store, {{1, 0, 0, 1}}},
        {b->View.get(), LoadAction::Clear, StoreAction::Store, {{1, 0, 0, 1}}}};
    auto encoder = command->BeginRenderPass({.Colors = attachments, .Width = 8, .Height = 8}).Unwrap();
    command->EndRenderPass(std::move(encoder));
    for (auto& attachment : attachments) attachment.Load = LoadAction::Load;
    encoder = command->BeginRenderPass({.Colors = attachments, .Width = 8, .Height = 8}).Unwrap();
    command->EndRenderPass(std::move(encoder));
    for (auto& attachment : attachments) {
        attachment.Load = LoadAction::Clear;
        attachment.Clear = {{0, 1, 0, 1}};
    }
    encoder = command->BeginRenderPass({.Colors = attachments, .Width = 8, .Height = 8, .RenderArea = Rect{2, 2, 4, 4}}).Unwrap();
    command->EndRenderPass(std::move(encoder));
    const ResourceBarrierDescriptor toCopy = BarrierTextureDescriptor{a->Tex.get(), TextureState::RenderTarget, TextureState::CopySource};
    command->ResourceBarrier(std::span{&toCopy, 1});
    command->CopyTextureToBuffer(readback.get(), 0, a->Tex.get(), {0, 1, 0, 1});
    command->End();
    CommandBuffer* commands[]{command.get()};
    test::GpuWorkGuard gpuWork{Context};
    Context.Queue->Submit({.CmdBuffers = commands});
    Context.Queue->Wait();
    auto* data = static_cast<const uint8_t*>(readback->Map(0, rowPitch * 8));
    ASSERT_NE(data, nullptr);
    readback->InvalidateMappedRange({0, rowPitch * 8});
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const bool inside = x >= 2 && x < 6 && y >= 2 && y < 6;
            EXPECT_EQ(data[y * rowPitch + x * 4], inside ? 0 : 255);
            EXPECT_EQ(data[y * rowPitch + x * 4 + 1], inside ? 255 : 0);
        }
    readback->Unmap();
    allocator->Reset();
}

TEST_P(VulkanRenderScopeGpu, DepthOnlyDepthStencilReadonlyAndMsaa) {
    auto& device = *Context.Device;
    for (auto format : {TextureFormat::D32_FLOAT, TextureFormat::D32_FLOAT_S8_UINT}) {
        const auto usage = TextureUse::DepthStencilWrite | TextureUse::DepthStencilRead;
        const auto support = device.QueryTextureSupport({TextureDimension::Dim2D, format, usage});
        ASSERT_TRUE(support.Supported);
        for (uint32_t samples : {1u, 4u}) {
            if (!(support.SampleCounts.value() & samples)) continue;
            auto texture = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = samples, .Format = format, .Usage = usage}).Unwrap();
            auto write = device.CreateTextureView({texture.get(), TextureDimension::Dim2D, format, {0, 1, 0, 1}, TextureViewUsage::DepthWrite}).Unwrap();
            auto read = device.CreateTextureView({texture.get(), TextureDimension::Dim2D, format, {0, 1, 0, 1}, TextureViewUsage::DepthRead}).Unwrap();
            auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
            auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
            command->Begin();
            const ResourceBarrierDescriptor toWrite = BarrierTextureDescriptor{texture.get(), TextureState::Undefined, TextureState::DepthWrite};
            command->ResourceBarrier(std::span{&toWrite, 1});
            auto encoder = command->BeginRenderPass({.DepthStencil = RenderPassDepthStencilAttachment{write.get(), LoadAction::Clear, StoreAction::Store, LoadAction::Clear, StoreAction::Store, false, {0.5f, 7}}, .Width = 8, .Height = 8}).Unwrap();
            command->EndRenderPass(std::move(encoder));
            const ResourceBarrierDescriptor toRead = BarrierTextureDescriptor{texture.get(), TextureState::DepthWrite, TextureState::DepthRead};
            command->ResourceBarrier(std::span{&toRead, 1});
            encoder = command->BeginRenderPass({.DepthStencil = RenderPassDepthStencilAttachment{read.get(), LoadAction::Load, StoreAction::Store, LoadAction::Load, StoreAction::Store, true, {}}, .Width = 8, .Height = 8}).Unwrap();
            command->EndRenderPass(std::move(encoder));
            command->End();
            CommandBuffer* commands[]{command.get()};
            test::GpuWorkGuard gpuWork{Context};
            Context.Queue->Submit({.CmdBuffers = commands});
            Context.Queue->Wait();
            allocator->Reset();
        }
    }
}

TEST_P(VulkanRenderScopeGpu, NativeMipAndLayerRangesDriveFramebufferConstraints) {
    auto& device = *Context.Device;
    auto texture = device.CreateTexture({.Dim = TextureDimension::Dim2DArray, .Width = 16, .Height = 16, .DepthOrArraySize = 2, .MipLevels = 2, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget}).Unwrap();
    auto view = device.CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, SubresourceRange::All, 1, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    auto layered = device.CreateTextureView({texture.get(), TextureDimension::Dim2DArray, TextureFormat::RGBA8_UNORM, {0, 2, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor barrier = BarrierTextureDescriptor{texture.get(), TextureState::Undefined, TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&barrier, 1});
    const RenderPassColorAttachment attachment{view.get(), LoadAction::Clear, StoreAction::Store, {}};
    auto encoder = command->BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = 8, .Height = 8}).Unwrap();
    command->EndRenderPass(std::move(encoder));
    const RenderPassColorAttachment arrayAttachment{layered.get(), LoadAction::Clear, StoreAction::Store, {}};
    encoder = command->BeginRenderPass({.Colors = std::span{&arrayAttachment, 1}, .Width = 16, .Height = 16, .Layers = 2}).Unwrap();
    command->EndRenderPass(std::move(encoder));
    command->End();
    CommandBuffer* commands[]{command.get()};
    test::GpuWorkGuard gpuWork{Context};
    Context.Queue->Submit({.CmdBuffers = commands});
    Context.Queue->Wait();
    allocator->Reset();
}

INSTANTIATE_TEST_SUITE_P(Modes, VulkanRenderScopeGpu, testing::Values(FramebufferModeVulkan::Traditional, FramebufferModeVulkan::Imageless));

}  // namespace
}  // namespace radray::render::vulkan

#endif
