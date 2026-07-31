# RenderPassCacheKey / FramebufferCacheKey 的纯 CPU 行为 (相等性、哈希、descriptor 回读),
# 不需要 device。RenderPassRegistry 本身要真设备, 由 runtime 侧的集成测试覆盖。
radray_add_test(test_render_pass_registry SOURCES test_render_pass_registry.cpp LINK_LIBS radrayrender)
