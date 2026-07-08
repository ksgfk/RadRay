#pragma once

#include <type_traits>

#include <radray/hash.h>
#include <radray/nullable.h>
#include <radray/render/common.h>
#include <radray/types.h>

namespace radray::render {

/// Sampler 缓存的纯 POD key (仿 GraphicsPsoKey)。
///
/// 所有字段为标量, 无指针 / optional / span。构造时以 `SamplerKey{}` 清零, 再逐字段赋值,
/// 保证 padding 恒为 0, 从而可安全用于 PodHasher (byte-wise xxHash) 与 PodEqual (memcmp)。
/// SamplerDescriptor::Compare 是 std::optional, 在此展平为 HasCompare + Compare 两个标量。
struct SamplerKey {
    int32_t AddressS;
    int32_t AddressT;
    int32_t AddressR;
    int32_t MinFilter;
    int32_t MagFilter;
    int32_t MipmapFilter;
    float LodMin;
    float LodMax;
    uint32_t HasCompare;
    int32_t Compare;
    uint32_t AnisotropyClamp;
};

static_assert(std::is_trivially_copyable_v<SamplerKey>, "SamplerKey must be trivially copyable");

/// 从 SamplerDescriptor 构造清零的 POD key。
SamplerKey BuildSamplerKey(const SamplerDescriptor& desc) noexcept;

/// 采样器缓存 (对应 UE5 的 GTextureSamplerStateCache / 各 RHI 后端 sampler cache)。
///
/// 设计要点:
/// - 按 SamplerDescriptor 去重: 相同状态的 sampler 只创建一次。
/// - unique_ptr 永生持有: 一经创建即缓存到 app 生命周期结束, 从不单独释放。
///   因此 GetOrCreate 返回的裸指针在缓存存活期内【永不悬垂】, 材质快照可安全跨帧/跨线程持有。
/// - sampler 是纯状态对象 (无数据), 组合数有限, 永生缓存无实际内存压力。
class SamplerCache {
public:
    explicit SamplerCache(Device* device) noexcept;
    SamplerCache(const SamplerCache&) = delete;
    SamplerCache(SamplerCache&&) = delete;
    SamplerCache& operator=(const SamplerCache&) = delete;
    SamplerCache& operator=(SamplerCache&&) = delete;
    ~SamplerCache() noexcept = default;

    /// 按 descriptor 去重取 sampler。命中返回缓存指针; 未命中创建并永生缓存。
    /// device 为空 / 创建失败返回 nullptr。返回的指针在本缓存存活期内稳定不悬垂。
    Nullable<Sampler*> GetOrCreate(const SamplerDescriptor& desc) noexcept;

private:
    Device* _device{nullptr};
    unordered_map<SamplerKey, unique_ptr<Sampler>, PodHasher<SamplerKey>, PodEqual<SamplerKey>> _cache;
};

}  // namespace radray::render
