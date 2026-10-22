// Two RootConstants parameters only exist on D3D12: SPIR-V allows a single push constant block,
// so the declarations the policy names are DXIL-only. The policy itself stays on both lanes (it is
// a translation-unit fact and entries must not be conditionally compiled); on SPIR-V it simply
// names no declaration, so that lane publishes no push block at all.
#define RS \
    "RootConstants(num32BitConstants=16, b0, space=0)," \
    "RootConstants(num32BitConstants=4, b1, space=0)"

#if !defined(__spirv__)
struct ObjectData {
    float4x4 Transform;
};
struct MaterialData {
    float4 Tint;
};

// Root constants are published from the policy, but the policy parameter still has to name a real
// declaration: that declaration is what the push handle is keyed on.
ConstantBuffer<ObjectData> ObjectConstants : register(b0, space0);
ConstantBuffer<MaterialData> MaterialConstants : register(b1, space0);
#endif

[shader("vertex")]
[RootSignature(RS)]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
[RootSignature(RS)]
float4 PSMain() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
