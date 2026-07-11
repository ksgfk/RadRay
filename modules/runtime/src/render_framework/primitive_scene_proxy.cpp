#include <radray/runtime/render_framework/primitive_scene_proxy.h>

#include <atomic>

namespace radray {

namespace {
std::atomic<uint64_t> gNextPrimitiveProxyGeneration{1};
}

PrimitiveSceneProxy::PrimitiveSceneProxy() noexcept
    : _generation(gNextPrimitiveProxyGeneration.fetch_add(1, std::memory_order_relaxed)) {}

PrimitiveSceneProxy::~PrimitiveSceneProxy() noexcept = default;

}  // namespace radray
