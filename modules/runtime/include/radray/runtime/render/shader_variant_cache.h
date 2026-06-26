#pragma once

#include <optional>
#include <string_view>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/shader_identity.h>
#include <radray/runtime/render/shader.h>
#include <radray/runtime/render/keyword_set.h>

namespace radray {
class GpuSystem;
}

namespace radray::srp {

/// 一个已编译的 shader 变体。全部是现成 RHI 对象(GpuSystem 缓存拥有,本结构仅引用)。
/// 对应 srp_runtime_architecture.md §8:三级缓存产物即 render:: 对象,底层不动。
struct ShaderVariant {
    CompiledShaderEntry VS{};
    std::optional<CompiledShaderEntry> PS{};  ///< depth-only pass 无 PS
    render::RootSignature* RootSig{nullptr};
    RootSignatureLayoutKey RootLayout{};

    bool IsValid() const noexcept { return VS.Target != nullptr && RootSig != nullptr; }
};

/// Shader 变体编译缓存。key = (ShaderId, LightMode, KeywordSet)。
/// 【MaterialInstanceId 不进 key】—— 这是 SRP Batcher 可批的根:海量实例共享变体。
/// 命中直接返回;miss 时按 Shader 的对应 pass 源编译 VS(+PS)、取共享 RootSignature、入缓存。
///
/// 本类是"机器",不拥有内容:编译产物归 GpuSystem 的 ShaderCache/RSCache;
/// 本类只缓存"(shader,lightMode,keywords) → 已解析的 ShaderVariant"这层去重映射。
class ShaderVariantCache {
public:
    explicit ShaderVariantCache(GpuSystem* gpu) noexcept : _gpu(gpu) {}
    ShaderVariantCache(const ShaderVariantCache&) = delete;
    ShaderVariantCache& operator=(const ShaderVariantCache&) = delete;

    /// 取或编译某变体。失败(shader 没这个 lightMode 的 pass、或编译失败)返回 nullptr。
    /// 返回的指针稳定(缓存以 node 容器持有),调用方勿释放。
    const ShaderVariant* Get(const Shader& shader, std::string_view lightMode, const KeywordSet& keywords);

    void Clear() noexcept { _cache.clear(); }
    size_t Size() const noexcept { return _cache.size(); }

private:
    struct Key {
        uint64_t ShaderId{0};
        string LightMode;
        KeywordSet Keywords;

        friend bool operator==(const Key&, const Key&) noexcept = default;
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept;
    };

    ShaderVariant Compile(const Shader& shader, std::string_view lightMode, const KeywordSet& keywords);

    GpuSystem* _gpu;
    unordered_map<Key, ShaderVariant, KeyHash> _cache;
};

}  // namespace radray::srp
