#pragma once

#include <optional>
#include <string_view>
#include <utility>

#include <radray/types.h>

namespace radray::srp {

/// 一个 shader pass 的标签集:照搬 Unity `Pass { Tags { ... } }` 的原貌,
/// 一排 string→string 键值。`"LightMode"` 只是其中一个约定 key,
/// 旁边还可有 `"Queue"` / `"RenderType"` 等。数量极少(通常 1~3 条),线性查足够。
/// 设计依据:srp_runtime_design.md §2b(不引入强类型 LightModeId,默认最简 string kv)。
struct TagSet {
    vector<std::pair<string, string>> Tags;

    /// 约定 key:标识该 pass 属于哪个 LightMode。
    static constexpr std::string_view LightModeKey = "LightMode";

    std::optional<std::string_view> Find(std::string_view key) const noexcept {
        for (const auto& [k, v] : Tags) {
            if (k == key) {
                return std::string_view{v};
            }
        }
        return std::nullopt;
    }

    /// 取本 pass 的 LightMode 标签值(没有则返回空)。
    std::optional<std::string_view> LightMode() const noexcept { return Find(LightModeKey); }
};

/// Pass 想要的 LightMode,按优先级从高到低排列(对齐 SRP `SetShaderPassName(i, tag)`,
/// index 即优先级)。解析时遍历此列表,第一个被某 shader pass 的 `"LightMode"` 命中者即选中。
/// 全不命中 → 该 renderer 不进 RendererList(relevance 失败)。
using WantedLightModes = vector<string>;

}  // namespace radray::srp
