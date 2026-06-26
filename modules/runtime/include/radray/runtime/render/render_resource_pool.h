#pragma once

#include <cstdint>
#include <string_view>

#include <radray/types.h>
#include <radray/render/common.h>

namespace radray::srp {

/// 跨 pass 共享的瞬态纹理池(最小化)。对应 Unity 的帧资源 / UE5 RDG 资源句柄的最小落点。
///
/// 做且只做三件事——正是单个 pass 过去手搓的那三件:
///  1. 拥有并复用瞬态纹理:按 desc 匹配,尺寸/格式变了就重建(自动解决 resize)。
///  2. 跟踪每个资源当前的 TextureStates。
///  3. 按名字交接 + 在显式 transition 时发出正确的 barrier。
///
/// 这不是 RDG:无资源别名、无依赖图、无 pass 重排、不【自动推导】transition。
/// transition 由 pass 显式调用,池子只跟踪态并发 barrier,不猜。
///
/// 按 (name, flight) 索引:沿用"runtime 复用某条 flight 槽位前会等其 fence"的不变量,
/// 故同名资源每条 flight 各持一份,复用前重建上一份是安全的。
class RenderResourcePool {
public:
    RenderResourcePool() noexcept = default;
    ~RenderResourcePool() noexcept = default;
    RenderResourcePool(const RenderResourcePool&) = delete;
    RenderResourcePool(RenderResourcePool&&) = delete;
    RenderResourcePool& operator=(const RenderResourcePool&) = delete;
    RenderResourcePool& operator=(RenderResourcePool&&) = delete;

    /// 取(创建或复用)本 flight 下名为 name 的瞬态纹理。desc 匹配则复用,不匹配则重建。
    render::Texture* Acquire(
        std::string_view name,
        uint32_t flight,
        const render::TextureDescriptor& desc,
        render::Device& device);

    /// 查已存在的资源(供下游 pass 接力)。不存在返回 nullptr。
    render::Texture* Find(std::string_view name, uint32_t flight) const noexcept;

    /// 取(创建或复用)某资源的一个 view,生命周期归池。viewDesc.Target 由池托管。
    render::TextureView* GetView(
        std::string_view name,
        uint32_t flight,
        const render::TextureViewDescriptor& viewDesc,
        render::Device& device);

    /// 把资源迁到 state:跟踪态 != 目标态时发一条 barrier 并更新跟踪态。
    void Transition(
        std::string_view name,
        uint32_t flight,
        render::TextureStates state,
        render::CommandBuffer& cmd);

    /// 释放全部资源。须在 GPU idle、device 仍有效时调用。
    void Clear() noexcept;

private:
    struct ViewEntry {
        render::TextureViewDescriptor Desc{};
        unique_ptr<render::TextureView> View{};
    };

    struct Entry {
        string Name{};
        uint32_t Flight{0};
        render::TextureDescriptor Desc{};
        unique_ptr<render::Texture> Tex{};
        render::TextureStates State{render::TextureState::Undefined};
        vector<ViewEntry> Views{};
    };

    Entry* FindEntry(std::string_view name, uint32_t flight) noexcept;
    const Entry* FindEntry(std::string_view name, uint32_t flight) const noexcept;

    vector<Entry> _entries{};
};

}  // namespace radray::srp
