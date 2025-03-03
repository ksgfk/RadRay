// #pragma once

// namespace radray::runtime {

// class ConstantBufferPool {
// public:
//     struct Ref {
//         render::Buffer* Buffer;
//         uint64_t Offset;
//     };

//     ConstantBufferPool(render::Device* device, uint64_t initSize = 16384) noexcept;

//     Ref Allocate(uint64_t size) noexcept;
//     void Clear() noexcept;

// private:
//     class Block {
//     public:
//         shared_ptr<render::Buffer> buf;
//         uint64_t capacity;
//         uint64_t size;
//     };

//     shared_ptr<render::Buffer> CreateCBuffer(uint64_t size) noexcept;

//     render::Device* _device;
//     vector<Block> _blocks;
//     uint64_t _initSize;
// };

// class DescriptorSetPool {
// public:
//     explicit DescriptorSetPool(render::Device* device) noexcept;

//     shared_ptr<render::DescriptorSet> GetOrCreateDescriptorSet(
//         render::RootSignature* rootSig,
//         uint32_t set,
//         std::span<render::ResourceView*> views) noexcept;

// private:
//     class PoolKey {
//     public:
//         vector<render::ResourceView*> views;

//         size_t operator()() const noexcept;
//     };

//     render::Device* _device;
//     unordered_map<PoolKey, shared_ptr<render::DescriptorSet>> _pool;
// };

// class ResourceTable {
// public:
//     explicit ResourceTable(render::RootSignature* rootSig) noexcept;

//     void SetConstantBufferData(std::string_view name, uint32_t index, std::span<const byte> data) noexcept;

//     void SetResource(std::string_view name, uint32_t index, shared_ptr<render::ResourceView> rv) noexcept;

// private:
//     struct RootConstSlot {
//         render::RootSignatureRootConstantSlotInfo Info;
//         size_t CbCacheStart;
//     };

//     struct CBufferSlot {
//         render::RootSignatureConstantBufferSlotInfo Info;
//         size_t CbCacheStart;
//     };

//     struct DescriptorLayoutIndex {
//         size_t Index;
//         size_t CbCacheStart;
//         vector<weak_ptr<render::ResourceView>> Views;
//     };

//     using Slot = std::variant<RootConstSlot, CBufferSlot, DescriptorLayoutIndex>;

//     unordered_map<string, Slot, StringHash, std::equal_to<>> _slots;
//     vector<render::DescriptorLayout> _descLayouts;
//     Memory _cbCache;
// };

// }  // namespace radray::runtime
