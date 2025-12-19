#pragma once

#include <span>
#include <string_view>

#include <radray/vertex_data.h>
#include <radray/structured_buffer.h>

#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>

namespace radray::render {

struct SemanticMapping {
    std::string_view Semantic{};
    uint32_t SemanticIndex{0};
    uint32_t Location{0};
    VertexFormat Format{VertexFormat::UNKNOWN};
};
std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept;

// --------------------------------------- CBuffer Utility ---------------------------------------
std::optional<StructuredBufferStorage> CreateCBufferStorage(const HlslShaderDesc& desc) noexcept;
std::optional<StructuredBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept;

struct TempCbufferAllocation {
    BufferView* View{nullptr};
    void* CpuPtr{nullptr};
    uint64_t Size{0};
    uint64_t OffsetInPage{0};
};

class SimpleTempCbufferAllocator {
public:
    struct Descriptor {
        uint64_t PageSize{256u * 1024u};
        uint32_t MinAlignment{256u};
        std::string_view NamePrefix{"TempCBuffer"};
    };

    SimpleTempCbufferAllocator() noexcept = default;
    explicit SimpleTempCbufferAllocator(Device* device) noexcept;
    SimpleTempCbufferAllocator(Device* device, const Descriptor& desc) noexcept;

    SimpleTempCbufferAllocator(const SimpleTempCbufferAllocator&) = delete;
    SimpleTempCbufferAllocator(SimpleTempCbufferAllocator&&) = delete;
    SimpleTempCbufferAllocator& operator=(const SimpleTempCbufferAllocator&) = delete;
    SimpleTempCbufferAllocator& operator=(SimpleTempCbufferAllocator&&) = delete;

    ~SimpleTempCbufferAllocator() noexcept;

    void Shutdown() noexcept;
    // Recycle all allocations made so far (views are destroyed; pages are kept and reused).
    void Reset() noexcept;

    Device* GetDevice() const noexcept { return _device; }
    uint32_t GetCBufferAlignment() const noexcept { return _cbAlign; }

    // Allocate a temporary constant-buffer slice. Returned View/CpuPtr stay valid until the frame is retired.
    std::optional<TempCbufferAllocation> Allocate(uint64_t size, uint32_t alignment = 0) noexcept;

    // Allocate + copy data into the mapped upload page.
    std::optional<TempCbufferAllocation> AllocateAndWrite(std::span<const byte> data, uint32_t alignment = 0) noexcept;

    template <class T>
    std::optional<TempCbufferAllocation> AllocateAndWrite(const T& value, uint32_t alignment = 0) noexcept {
        return AllocateAndWrite(std::span{reinterpret_cast<const byte*>(&value), sizeof(T)}, alignment);
    }

private:
    struct Page {
        unique_ptr<Buffer> BufferObj;
        void* Mapped{nullptr};
        uint64_t Capacity{0};
        uint64_t Offset{0};
    };

    uint64_t AlignUp(uint64_t v, uint64_t align) const noexcept;
    uint64_t GetAllocAlignment(uint32_t alignment) const noexcept;
    std::optional<TempCbufferAllocation> AllocateInternal(uint64_t size, uint32_t alignment) noexcept;
    Page* GetOrCreatePage(uint64_t minCapacity) noexcept;

    void DestroyViews(vector<unique_ptr<BufferView>>& views) noexcept;
    void DestroyPages(vector<unique_ptr<Page>>& pages) noexcept;

private:
    Device* _device{nullptr};
    Descriptor _desc{};
    uint32_t _cbAlign{256u};

    // Current-frame working set
    vector<unique_ptr<Page>> _currPages;
    vector<unique_ptr<BufferView>> _currViews;
    Page* _currPage{nullptr};

    // Reusable pages
    vector<unique_ptr<Page>> _freePages;
};

// -----------------------------------------------------------------------------------------------

// ----------------------------------- Root Signature Utility ------------------------------------
class RootSignatureDescriptorContainer {
public:
    struct SetElementData {
        uint32_t Slot{0};
        uint32_t Space{0};
        ResourceBindType Type{ResourceBindType::UNKNOWN};
        uint32_t Count{0};
        ShaderStages Stages{ShaderStage::UNKNOWN};
        uint32_t StaticSamplerOffset{0};
        uint32_t StaticSamplerCount{0};
    };

    struct DescriptorSetRange {
        size_t ElementOffset{0};
        size_t ElementCount{0};
    };

    class View {
    public:
        const RootSignatureDescriptor& Get() const noexcept { return _desc; }

    private:
        RootSignatureDescriptor _desc{};
        vector<RootSignatureRootDescriptor> _rootDescriptors;
        vector<RootSignatureSetElement> _elements;
        vector<SamplerDescriptor> _staticSamplers;
        vector<RootSignatureDescriptorSet> _descriptorSets;

        friend class RootSignatureDescriptorContainer;
    };

    RootSignatureDescriptorContainer() noexcept = default;
    explicit RootSignatureDescriptorContainer(const RootSignatureDescriptor& desc) noexcept;

    View MakeView() const noexcept;

    std::span<const RootSignatureRootDescriptor> GetRootDescriptors() const noexcept { return _rootDescriptors; }
    std::span<const SetElementData> GetElements() const noexcept { return _elements; }
    std::span<const DescriptorSetRange> GetDescriptorSets() const noexcept { return _descriptorSets; }
    const std::optional<RootSignatureConstant>& GetConstant() const noexcept { return _constant; }

private:
    vector<RootSignatureRootDescriptor> _rootDescriptors;
    vector<SetElementData> _elements;
    vector<SamplerDescriptor> _staticSamplers;
    vector<DescriptorSetRange> _descriptorSets;
    std::optional<RootSignatureConstant> _constant;
};
Nullable<unique_ptr<RootSignature>> CreateSerializedRootSignature(Device* device, std::span<const byte> data) noexcept;
std::optional<RootSignatureDescriptorContainer> CreateRootSignatureDescriptor(const HlslShaderDesc& desc) noexcept;
// -----------------------------------------------------------------------------------------------

}  // namespace radray::render
