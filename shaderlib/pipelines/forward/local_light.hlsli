#ifndef RADRAY_FORWARD_LOCAL_LIGHT_HLSLI
#define RADRAY_FORWARD_LOCAL_LIGHT_HLSLI
// Fixed 64-byte storage-buffer element, independently packed on the CPU.
struct ForwardLocalLight {
    float4 PositionRadius;
    float4 ColorType;
    float4 DirectionCosOuter;
    float4 Cone;
};
#endif
