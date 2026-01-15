#pragma once

#include <span>
#include <string_view>
#include <stdexcept>

#include <radray/allocator.h>
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
struct CBufferStorageRange {
    string Name;
    vector<StructuredBufferId> Roots;
    uint32_t BindPoint;
    uint32_t Space;
    size_t GlobalOffset;
    size_t SizeInBytes;
};

std::optional<StructuredBufferStorage> CreateCBufferStorage(const HlslShaderDesc& desc) noexcept;
std::optional<StructuredBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept;

class SimpleCBufferArena {
public:
    struct Descriptor {
        uint64_t BasicSize{256 * 256};
        uint64_t Alignment{256};
        uint64_t MaxResetSize{std::numeric_limits<uint64_t>::max()};
        string NamePrefix{};
    };

    struct Allocation {
        Buffer* Target{nullptr};
        void* Mapped{nullptr};
        uint64_t Offset{0};
        uint64_t Size{0};

        static constexpr Allocation Invalid() noexcept {
            return Allocation{};
        }
    };

    class Block {
    public:
        explicit Block(unique_ptr<Buffer> buf) noexcept;
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
        ~Block() noexcept;

    public:
        unique_ptr<Buffer> _buf;
        void* _mapped{nullptr};
        uint64_t _used{0};
    };

    SimpleCBufferArena(Device* device, const Descriptor& desc) noexcept;
    explicit SimpleCBufferArena(Device* device) noexcept;
    SimpleCBufferArena(const SimpleCBufferArena&) = delete;
    SimpleCBufferArena& operator=(const SimpleCBufferArena&) = delete;
    SimpleCBufferArena(SimpleCBufferArena&& other) noexcept;
    SimpleCBufferArena& operator=(SimpleCBufferArena&& other) noexcept;
    ~SimpleCBufferArena() noexcept;

    bool IsValid() const noexcept;

    void Destroy() noexcept;

    Allocation Allocate(uint64_t size) noexcept;

    void Reset() noexcept;

    void Clear() noexcept;

    friend void swap(SimpleCBufferArena& a, SimpleCBufferArena& b) noexcept;

public:
    Nullable<Block*> GetOrCreateBlock(uint64_t size) noexcept;

    Device* _device;
    vector<unique_ptr<Block>> _blocks;
    Descriptor _desc;
    uint64_t _minBlockSize{};
};

class SimpleCBufferUploader {
public:
    explicit SimpleCBufferUploader(const HlslShaderDesc& desc) noexcept;

public:
    StructuredBufferStorage _storage;
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

// -------------------------------------- Upload Utility -----------------------------------------

// -----------------------------------------------------------------------------------------------

}  // namespace radray::render
