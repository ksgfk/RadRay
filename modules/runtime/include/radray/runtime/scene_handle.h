#pragma once

// Scene 句柄的类型与归属契约见 docs/architecture/runtime.md。

#include <radray/sparse_set.h>

namespace radray {

class Scene;
struct MeshBatch;
struct PrimitiveSceneInfo;
class MaterialRenderProxy;

// 默认构造得到无效句柄；仅 Scene 可包装底层句柄，不记录 Scene 身份。
template <typename T>
class SceneHandle {
public:
    constexpr SceneHandle() noexcept = default;

    // 仅判断是否非空；元素是否仍存活须通过所属 Scene 查询。
    constexpr bool IsValid() const noexcept { return _handle.IsValid(); }
    constexpr uint32_t GetIndex() const noexcept { return _handle.Index; }
    constexpr uint32_t GetGeneration() const noexcept { return _handle.Generation; }
    constexpr auto operator<=>(const SceneHandle&) const noexcept = default;

private:
    friend class Scene;
    explicit constexpr SceneHandle(SparseSetHandle handle) noexcept : _handle(handle) {}

    SparseSetHandle _handle{};
};

using MeshBatchHandle = SceneHandle<MeshBatch>;
using PrimitiveHandle = SceneHandle<PrimitiveSceneInfo>;
using MaterialHandle = SceneHandle<MaterialRenderProxy>;

}  // namespace radray

template <typename T, typename CharT>
class fmt::formatter<radray::SceneHandle<T>, CharT> : public fmt::formatter<radray::SparseSetHandle, CharT> {
public:
    template <typename FormatContext>
    auto format(const radray::SceneHandle<T>& handle, FormatContext& ctx) const {
        return fmt::formatter<radray::SparseSetHandle, CharT>::format({handle.GetIndex(), handle.GetGeneration()}, ctx);
    }
};
