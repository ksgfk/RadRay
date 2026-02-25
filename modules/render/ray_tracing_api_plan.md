# Ray Tracing API Plan for `common.h` (Hybrid Model, No `RayTracingTier`)

## Summary
Add ray tracing API to `modules/render/include/radray/render/common.h` with:
- `AccelerationStructure` as first-class object.
- Shader table as plain `Buffer` regions (no `ShaderTable` object/tag).
- Dedicated `RayTracingCommandEncoder`.
- D3D12 + Vulkan first; Metal unsupported.

## Public API Changes

### 1. `ShaderStage`
Add:
- `RayGen`
- `Miss`
- `ClosestHit`
- `AnyHit`
- `Intersection`
- `Callable`

### 2. Enums and flags
- `ResourceBindType`: add `AccelerationStructure`.
- `BufferUse`: add `AccelerationStructure`, `Scratch`, `ShaderTable`.
- `BufferState`: add `AccelerationStructureBuildInput`, `AccelerationStructureBuildScratch`, `AccelerationStructureRead`, `ShaderTable`.
- `RenderObjectTag`: add `AccelerationStructure`, `RayTracingPipelineState`, `RayTracingCmdEncoder`.
- No `ShaderTable` tag.

### 3. New RT types/descriptors
- `AccelerationStructureType`, `AccelerationStructureBuildMode`, `AccelerationStructureBuildFlags`.
- `RayTracingTrianglesDescriptor`, `RayTracingAABBsDescriptor`, `RayTracingGeometryDesc`.
- `RayTracingInstanceDescriptor` with `AccelerationStructure* Blas`.
- `AccelerationStructureDescriptor`, `BuildBottomLevelASDescriptor`, `BuildTopLevelASDescriptor`.
- `RayTracingShaderEntry`, `RayTracingHitGroupDescriptor`, `RayTracingPipelineStateDescriptor`.
- `ShaderBindingTableRegion` (`Buffer* Target`, `Offset`, `Size`, `Stride`).
- `TraceRaysDescriptor` (`RayGen`, `Miss`, `HitGroup`, optional `Callable`, `Width`, `Height`, `Depth`).

### 4. Classes
Add:
- `AccelerationStructure : Resource`.
- `RayTracingPipelineState : PipelineState`.
- `RayTracingCommandEncoder : CommandEncoder`.

Do not add:
- `ShaderTable` class.

### 5. `Device` additions
- `CreateAccelerationStructure(const AccelerationStructureDescriptor&)`
- `CreateRayTracingPipelineState(const RayTracingPipelineStateDescriptor&)`

### 6. `CommandBuffer` / RT encoder additions
- `BeginRayTracingPass()`
- `EndRayTracingPass(...)`

On `RayTracingCommandEncoder`:
- `BuildBottomLevelAS(...)`
- `BuildTopLevelAS(...)`
- `BindRayTracingPipelineState(...)`
- `TraceRays(const TraceRaysDescriptor&)`

## `DeviceDetail` (No `RayTracingTier`)
Add only:
- `bool IsRayTracingSupported{false}`
- `uint32_t MaxRayRecursionDepth{0}`
- `uint32_t ShaderTableAlignment{0}`
- `uint32_t AccelerationStructureAlignment{0}`
- `uint32_t AccelerationStructureScratchAlignment{0}`

## Validation and Utility
Add declarations/implementations:
- `ValidateAccelerationStructureDescriptor(...)`
- `ValidateBuildBottomLevelASDescriptor(...)`
- `ValidateBuildTopLevelASDescriptor(...)`
- `ValidateTraceRaysDescriptor(...)`

Extend `format_as(...)` for:
- new RT enums
- new `RenderObjectTag` values

## Tests
- Reject invalid scratch/SBT usage and alignment.
- Reject invalid TLAS instances (null BLAS).
- Reject invalid `TraceRays` setup (missing pipeline, bad regions, zero dims).
- Accept minimal valid BLAS+TLAS+TraceRays path.

## Assumptions
- Buffer-backed SBT is intentional.
- AS remains strongly typed for safety/validation.
- API is additive and backward-compatible.
