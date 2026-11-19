#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <radray/gpu/types.h>
#include <radray/nullable.h>
#include <radray/runtime/upload_arena.h>
#include <radray/types.h>

namespace radray {

struct TransientParameterBinding {
    uint32_t BindingId;
    uint32_t ArrayElement;
    gpu::ShaderParameterValue Value;

    friend bool operator==(const TransientParameterBinding&, const TransientParameterBinding&) noexcept = default;
};

class TransientParameterSetPool final {
public:
    explicit TransientParameterSetPool(gpu::Device& device) noexcept;
    TransientParameterSetPool(const TransientParameterSetPool&) = delete;
    TransientParameterSetPool& operator=(const TransientParameterSetPool&) = delete;

    // The shared set is immutable for this slot iteration. Referenced resources must outlive BeginFrame's reset.
    Nullable<gpu::ShaderParameterSet*> AcquireParameterSet(const shared_ptr<gpu::BindingGroupLayout>& layout,
                                                          std::span<const TransientParameterBinding> bindings);
    Nullable<gpu::ShaderParameterSet*> AcquireUninitializedParameterSet(const shared_ptr<gpu::BindingGroupLayout>& layout);
    // Call only after GPU completion and command allocator reset.
    void BeginFrame() noexcept;

private:
    struct FilledEntry {
        uint32_t Offset{0};
        uint32_t Count{0};
        size_t Hash{0};
        size_t Bucket{0};
        gpu::ShaderParameterSet* Set{nullptr};
    };
    struct Group {
        shared_ptr<gpu::BindingGroupLayout> Layout;
        vector<unique_ptr<gpu::ShaderParameterSet>> Sets;
        vector<TransientParameterBinding> KeyBindings;
        vector<FilledEntry> Entries;
        vector<uint32_t> Buckets;
        array<size_t, 8> Peaks{};
        size_t Used{0};
    };
    Nullable<gpu::ShaderParameterSet*> AcquireUninitialized(Group& group);

    gpu::Device& _device;
    unordered_map<gpu::BindingGroupLayout*, Group> _groups;
    vector<TransientParameterBinding> _scratch;
    size_t _windowIndex{0};
};

std::optional<UploadAllocation> AllocateDynamicUniform(UploadArena& arena, std::span<const byte> bytes);

}  // namespace radray
