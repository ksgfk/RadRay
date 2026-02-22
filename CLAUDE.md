# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

Dependencies must be fetched first (requires PowerShell):
```
pwsh fetch_sdks.ps1
pwsh fetch_third_party.ps1
```

Configure and build (macOS):
```
cmake --preset macos-arm64-debug
cmake --build build_debug
```

Configure and build (Windows):
```
cmake --preset win-x64-debug
cmake --build build_debug --config Debug
```

Run all tests:
```
ctest --test-dir build_debug
```

Run a single test:
```
ctest --test-dir build_debug -R test_buddy_alloc
```

Build output goes to `build_debug/_build/Debug` (or `build_debug/_build/` on single-config generators like Ninja).

## Architecture

### Render Backend Abstraction

The core architectural pattern is a virtual interface layer in `modules/render/include/radray/render/common.h` (~1180 lines) that defines abstract base classes for all GPU objects. Three backends implement these interfaces:

- `modules/render/src/d3d12/d3d12_impl.cpp` + `backend/d3d12_impl.h`
- `modules/render/src/vk/vulkan_impl.cpp` + `backend/vulkan_impl.h`
- `modules/render/src/metal/metal_impl.mm` + `backend/metal_impl.h`

Key classes in the hierarchy: `Device` (factory for all GPU objects), `CommandQueue`, `CommandBuffer`, `CommandEncoder` → `GraphicsCommandEncoder` / `ComputeCommandEncoder`, `SwapChain`, `Buffer`, `Texture`, `RootSignature`, `DescriptorSet`, `BindlessArray`.

All creation methods on `Device` return `Nullable<unique_ptr<T>>` — the project's Result/Option pattern. `Nullable<T>` wraps pointer-like types; use `.HasValue()` to check, `.Unwrap()` to extract (throws on null), `.Release()` for no-throw extraction.

Entry point: `CreateDevice(DeviceDescriptor)` where `DeviceDescriptor` is a `std::variant<D3D12DeviceDescriptor, MetalDeviceDescriptor, VulkanDeviceDescriptor>`.

### Shader Pipeline

Shaders are written in HLSL and compiled at runtime:
- DXC compiles HLSL → DXIL (D3D12) or SPIR-V (Vulkan) via `modules/render/include/radray/render/dxc.h`
- SPIR-V Cross converts SPIR-V → MSL (Metal) via `modules/render/include/radray/render/spvc.h`
- Shared shader library in `shaderlib/` (BSDF, lighting, utilities)

### Module Dependencies

```
core (standalone) → window (core) → render (core) → imgui (core + window + render)
```

Library targets: `radraycore`, `radraywindow`, `radrayrender`, `radrayimgui`.

### Metal Backend Buffer Binding Layout

`MetalMaxVertexInputBindings = 16` (defined in `common.h`) is the key constant that drives the entire layout.

**Graphics stages (Vertex/Fragment):**
```
buffer(0-15)  — vertex input buffers (reserved, even for fragment stage)
buffer(16)    — push constants (setVertexBytes / setFragmentBytes)
buffer(17+)   — argument buffers, one per descriptor set (set N → buffer(N + 17))
```

**Compute stage:** no offset — resources use their original binding indices directly.

**SPIR-V → MSL remapping** (`ConvertSpirvToMsl` in `modules/render/src/spvc.cpp`):
- With argument buffers (`UseArgumentBuffers = true`): each descriptor set gets `kArgumentBufferBinding` mapped to `msl_buffer = descSet + MetalMaxVertexInputBindings + 1`. Push constants at `msl_buffer = MetalMaxVertexInputBindings`.
- Without argument buffers: buffer resources offset by `+MetalMaxVertexInputBindings`, textures/samplers keep original indices.

**HLSL register → Metal index (with argument buffers):**
| HLSL | Metal (graphics) | Metal (compute) |
|---|---|---|
| `[[vk::push_constant]]` | `buffer(16)` | `buffer(0)` (no offset) |
| `register(*, space0)` | argument buffer at `buffer(17)` | direct binding |
| `register(*, space1)` | argument buffer at `buffer(18)` | direct binding |
| vertex attributes | `buffer(0-15)` / `attribute(N)` | N/A |

**Runtime binding** (`metal_impl.mm`):
- `BindVertexBuffer` → `setVertexBuffer:atIndex:` with index 0-15
- `SetPushConstant` → `set{Vertex,Fragment}Bytes:atIndex:` with `slot + MetalMaxVertexInputBindings`
- `SetDescriptorSet` → `set{Vertex,Fragment}Buffer:atIndex:` with `slot + MetalMaxVertexInputBindings + 1`
- Root descriptors are NOT supported in the Metal backend

### Key Patterns

- **EnumFlags**: Bitwise enum wrappers via `EnumFlags<T>` with `is_flags<T>` trait specialization (see `enum_flags.h`)
- **RAII**: All GPU resources are non-copyable, non-movable (`RenderBase` deletes copy/move)
- **Signal/Slot**: Window events use sigslot library for event-driven callbacks
- **Frame-in-Flight**: Multi-buffered rendering with Fence/Semaphore synchronization
- **ImGuiApplication**: Base class in `modules/imgui/` for quick prototyping with any backend

## Language & Platform Notes

- C++20 with concepts, ranges; Objective-C++ (.mm) for Metal and Cocoa on macOS
- Comments are mixed Chinese and English
- CMake helper functions in `cmake/Utility.cmake`: `radray_set_build_path`, `radray_optimize_flags_library`, `radray_optimize_flags_binary`, `radray_example_files`
- Tests use Google Test with `gtest_discover_tests()`, each in its own subdirectory under `tests/`
