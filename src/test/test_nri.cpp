#include <array>
#include <vector>
#include <memory>

#include <radray/logger.h>
#include <radray/window/glfw_window.h>

#include <NRI.h>
#include <NRIDescs.h>
#include <Extensions/NRIHelper.h>
#include <Extensions/NRISwapChain.h>
#include <Extensions/NRIStreamer.h>
#include <Extensions/NRIDeviceCreation.h>

#define NRI_ABORT_ON_FAILURE(result)                   \
    do {                                               \
        if ((result) != (nri::Result::SUCCESS)) {      \
            RADRAY_ABORT("fail code {}", (int)result); \
        }                                              \
    } while (0);

constexpr int BUFFERED_FRAME_MAX_NUM = 3;

struct NRIInterface : public nri::CoreInterface, public nri::HelperInterface, public nri::SwapChainInterface, public nri::StreamerInterface {};

struct Frame {
    nri::CommandAllocator* commandAllocator;
    nri::CommandBuffer* commandBuffer;
    nri::Descriptor* constantBufferView;
    nri::DescriptorSet* constantBufferDescriptorSet;
    uint64_t constantBufferViewOffset;
};

struct BackBuffer {
    nri::Descriptor* colorAttachment;
    nri::Texture* texture;
};

class Test {
public:
    Test() {
        _glfw = std::make_unique<radray::window::GlfwWindow>("test nri", 1280, 720);
        m_NRIWindow.windows.hwnd = reinterpret_cast<void*>(_glfw->GetNativeHandle());
    };
    ~Test();

    bool Initialize(nri::GraphicsAPI graphicsAPI);
    void Run();

private:
    std::unique_ptr<radray::window::GlfwWindow> _glfw;
    nri::MemoryAllocatorInterface m_MemoryAllocatorInterface = {};

    nri::Window m_NRIWindow = {};

    NRIInterface NRI{};
    nri::Device* m_Device = nullptr;
    nri::CommandQueue* m_CommandQueue = nullptr;
    nri::Fence* m_FrameFence;
    nri::SwapChain* m_SwapChain;
    nri::DescriptorPool* m_DescriptorPool = nullptr;
    std::vector<BackBuffer> m_SwapChainBuffers;
    std::array<Frame, BUFFERED_FRAME_MAX_NUM> m_Frames = {};
};

Test::~Test() {
    NRI.WaitForIdle(*m_CommandQueue);
    for (Frame& frame : m_Frames) {
        NRI.DestroyCommandBuffer(*frame.commandBuffer);
        NRI.DestroyCommandAllocator(*frame.commandAllocator);
        NRI.DestroyDescriptor(*frame.constantBufferView);
    }
    for (BackBuffer& backBuffer : m_SwapChainBuffers) {
        NRI.DestroyDescriptor(*backBuffer.colorAttachment);
    }
    NRI.DestroyDescriptorPool(*m_DescriptorPool);
    NRI.DestroySwapChain(*m_SwapChain);
    NRI.DestroyFence(*m_FrameFence);
    nri::nriDestroyDevice(*m_Device);
    _glfw->Destroy();
}

bool Test::Initialize(nri::GraphicsAPI graphicsAPI) {
    nri::AdapterDesc bestAdapterDesc = {};
    uint32_t adapterDescsNum = 1;
    NRI_ABORT_ON_FAILURE(nri::nriEnumerateAdapters(&bestAdapterDesc, adapterDescsNum));
    // Device
    nri::DeviceCreationDesc deviceCreationDesc{};
    deviceCreationDesc.graphicsAPI = graphicsAPI;
    deviceCreationDesc.enableAPIValidation = true;
    deviceCreationDesc.enableNRIValidation = true;
    deviceCreationDesc.spirvBindingOffsets = {0, 0, 0, 0};
    deviceCreationDesc.adapterDesc = &bestAdapterDesc;
    deviceCreationDesc.memoryAllocatorInterface = m_MemoryAllocatorInterface;
    deviceCreationDesc.callbackInterface = {
        .MessageCallback = [](nri::Message messageType, const char* file, uint32_t line, const char* message, void* userArg) {
            switch (messageType) {
                case nri::Message::TYPE_INFO: RADRAY_INFO_LOG("{}:{}, {}", file, line, message); break;
                case nri::Message::TYPE_WARNING: RADRAY_WARN_LOG("{}:{}, {}", file, line, message); break;
                case nri::Message::TYPE_ERROR: RADRAY_ERR_LOG("at: {}:{}, {}", file, line, message); break;
                default: break;
            } },
        .AbortExecution = [](void* userArg) { RADRAY_ABORT("abort"); },
        .userArg = nullptr};
    NRI_ABORT_ON_FAILURE(nri::nriCreateDevice(deviceCreationDesc, m_Device));
    // NRI
    NRI_ABORT_ON_FAILURE(nri::nriGetInterface(*m_Device, NRI_INTERFACE(nri::CoreInterface), (nri::CoreInterface*)&NRI));
    NRI_ABORT_ON_FAILURE(nri::nriGetInterface(*m_Device, NRI_INTERFACE(nri::HelperInterface), (nri::HelperInterface*)&NRI));
    NRI_ABORT_ON_FAILURE(nri::nriGetInterface(*m_Device, NRI_INTERFACE(nri::StreamerInterface), (nri::StreamerInterface*)&NRI));
    NRI_ABORT_ON_FAILURE(nri::nriGetInterface(*m_Device, NRI_INTERFACE(nri::SwapChainInterface), (nri::SwapChainInterface*)&NRI));
    // Command queue
    NRI_ABORT_ON_FAILURE(NRI.GetCommandQueue(*m_Device, nri::CommandQueueType::GRAPHICS, m_CommandQueue));
    // Fences
    NRI_ABORT_ON_FAILURE(NRI.CreateFence(*m_Device, 0, m_FrameFence));
    // Swap chain
    nri::Format swapChainFormat;
    {
        nri::SwapChainDesc swapChainDesc = {};
        swapChainDesc.window = m_NRIWindow;
        swapChainDesc.commandQueue = m_CommandQueue;
        swapChainDesc.format = nri::SwapChainFormat::BT709_G22_8BIT;
        swapChainDesc.verticalSyncInterval = 1;
        swapChainDesc.width = (uint16_t)_glfw->GetSize().x();
        swapChainDesc.height = (uint16_t)_glfw->GetSize().y();
        swapChainDesc.textureNum = BUFFERED_FRAME_MAX_NUM;
        NRI_ABORT_ON_FAILURE(NRI.CreateSwapChain(*m_Device, swapChainDesc, m_SwapChain));

        uint32_t swapChainTextureNum;
        nri::Texture* const* swapChainTextures = NRI.GetSwapChainTextures(*m_SwapChain, swapChainTextureNum);
        swapChainFormat = NRI.GetTextureDesc(*swapChainTextures[0]).format;

        for (uint32_t i = 0; i < swapChainTextureNum; i++) {
            nri::Texture2DViewDesc textureViewDesc = {swapChainTextures[i], nri::Texture2DViewType::COLOR_ATTACHMENT, swapChainFormat};

            nri::Descriptor* colorAttachment;
            NRI_ABORT_ON_FAILURE(NRI.CreateTexture2DView(textureViewDesc, colorAttachment));

            const BackBuffer backBuffer = {colorAttachment, swapChainTextures[i]};
            m_SwapChainBuffers.push_back(backBuffer);
        }
    }
    // Buffered resources
    for (Frame& frame : m_Frames) {
        NRI_ABORT_ON_FAILURE(NRI.CreateCommandAllocator(*m_CommandQueue, frame.commandAllocator));
        NRI_ABORT_ON_FAILURE(NRI.CreateCommandBuffer(*frame.commandAllocator, frame.commandBuffer));
    }
    {  // Descriptor pool
        nri::DescriptorPoolDesc descriptorPoolDesc = {};
        descriptorPoolDesc.descriptorSetMaxNum = BUFFERED_FRAME_MAX_NUM + 1;
        descriptorPoolDesc.constantBufferMaxNum = BUFFERED_FRAME_MAX_NUM;
        descriptorPoolDesc.textureMaxNum = 1;
        descriptorPoolDesc.samplerMaxNum = 1;

        NRI_ABORT_ON_FAILURE(NRI.CreateDescriptorPool(*m_Device, descriptorPoolDesc, m_DescriptorPool));
    }
    return true;
}

void Test::Run() {
    uint64_t frameIndex = 0;
    while (!_glfw->ShouldClose()) {
        radray::window::GlobalPollEvents();
        auto size = _glfw->GetSize();

        const uint32_t bufferedFrameIndex = frameIndex % BUFFERED_FRAME_MAX_NUM;
        const Frame& frame = m_Frames[bufferedFrameIndex];
        if (frameIndex >= BUFFERED_FRAME_MAX_NUM) {
            NRI.Wait(*m_FrameFence, 1 + frameIndex - BUFFERED_FRAME_MAX_NUM);
            NRI.ResetCommandAllocator(*frame.commandAllocator);
        }
        const uint32_t currentTextureIndex = NRI.AcquireNextSwapChainTexture(*m_SwapChain);
        BackBuffer& currentBackBuffer = m_SwapChainBuffers[currentTextureIndex];

        nri::TextureBarrierDesc textureBarrierDescs = {};
        textureBarrierDescs.texture = currentBackBuffer.texture;
        textureBarrierDescs.after = {nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT};
        textureBarrierDescs.arraySize = 1;
        textureBarrierDescs.mipNum = 1;
        // Record
        nri::CommandBuffer* commandBuffer = frame.commandBuffer;
        NRI.BeginCommandBuffer(*commandBuffer, m_DescriptorPool);
        nri::BarrierGroupDesc barrierGroupDesc = {};
        barrierGroupDesc.textureNum = 1;
        barrierGroupDesc.textures = &textureBarrierDescs;
        NRI.CmdBarrier(*commandBuffer, barrierGroupDesc);
        nri::AttachmentsDesc attachmentsDesc = {};
        attachmentsDesc.colorNum = 1;
        attachmentsDesc.colors = &currentBackBuffer.colorAttachment;
        NRI.CmdBeginRendering(*commandBuffer, attachmentsDesc);
        {
            nri::ClearDesc clearDesc = {};
            clearDesc.attachmentContentType = nri::AttachmentContentType::COLOR;
            clearDesc.value.color32f = {1.0f, 1.0f, 0.0f, 1.0f};
            NRI.CmdClearAttachments(*commandBuffer, &clearDesc, 1, nullptr, 0);
            clearDesc.value.color32f = {0.46f, 0.72f, 0.0f, 1.0f};
            nri::Rect rects[2];
            rects[0] = {0, 0, (uint16_t)size.x(), (uint16_t)size.y()};
            rects[1] = {0, 0, (uint16_t)size.x(), (uint16_t)size.y()};
            NRI.CmdClearAttachments(*commandBuffer, &clearDesc, 1, rects, 2);
            NRI.CmdEndRendering(*commandBuffer);
            textureBarrierDescs.before = textureBarrierDescs.after;
            textureBarrierDescs.after = {nri::AccessBits::UNKNOWN, nri::Layout::PRESENT};
            NRI.CmdBarrier(*commandBuffer, barrierGroupDesc);
        }
        NRI.EndCommandBuffer(*commandBuffer);
        {  // Submit
            nri::QueueSubmitDesc queueSubmitDesc = {};
            queueSubmitDesc.commandBuffers = &frame.commandBuffer;
            queueSubmitDesc.commandBufferNum = 1;
            NRI.QueueSubmit(*m_CommandQueue, queueSubmitDesc);
        }
        NRI.QueuePresent(*m_SwapChain);
        {
            nri::FenceSubmitDesc signalFence = {};
            signalFence.fence = m_FrameFence;
            signalFence.value = 1 + frameIndex;
            nri::QueueSubmitDesc queueSubmitDesc = {};
            queueSubmitDesc.signalFences = &signalFence;
            queueSubmitDesc.signalFenceNum = 1;
            NRI.QueueSubmit(*m_CommandQueue, queueSubmitDesc);
        }
        frameIndex++;
    }
}

int main() {
    radray::window::GlobalInit();
    {
        Test test{};
        test.Initialize(nri::GraphicsAPI::D3D12);
        test.Run();
    }
    radray::window::GlobalTerminate();
    return 0;
}
