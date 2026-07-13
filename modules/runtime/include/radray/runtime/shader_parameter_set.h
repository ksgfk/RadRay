#pragma once

#include <span>

#include <radray/render/common.h>
#include <radray/types.h>

namespace radray {

class ShaderParameterSet {
public:
    struct ConstantEntry {
        string Name;
        vector<byte> Bytes;
    };
    struct ResourceEntry {
        string Name;
        render::ResourceView* Value{nullptr};
    };
    struct SamplerEntry {
        string Name;
        render::Sampler* Value{nullptr};
    };

    void SetConstant(std::string_view name, std::span<const byte> bytes);
    void SetResource(std::string_view name, render::ResourceView* resource) noexcept;
    void SetSampler(std::string_view name, render::Sampler* sampler) noexcept;
    void ClearConstant(std::string_view name) noexcept;
    void ClearResource(std::string_view name) noexcept;
    void ClearSampler(std::string_view name) noexcept;
    void Clear() noexcept;

    Nullable<const ConstantEntry*> FindConstant(std::string_view name) const noexcept;
    Nullable<render::ResourceView*> FindResource(std::string_view name) const noexcept;
    Nullable<render::Sampler*> FindSampler(std::string_view name) const noexcept;

    const vector<ConstantEntry>& Constants() const noexcept { return _constants; }
    const vector<ResourceEntry>& Resources() const noexcept { return _resources; }
    const vector<SamplerEntry>& Samplers() const noexcept { return _samplers; }
    uint64_t GetRevision() const noexcept { return _revision; }

private:
    vector<ConstantEntry> _constants;
    vector<ResourceEntry> _resources;
    vector<SamplerEntry> _samplers;
    uint64_t _revision{1};
};

}  // namespace radray
