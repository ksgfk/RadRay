struct NestedLeaf {
    float3 Direction;
    float Weight;
};

struct NestedArray {
    NestedLeaf Values[2];
};

struct NestedRoot {
    float4x4 Transform;
    NestedArray Data;
};

ConstantBuffer<NestedRoot> Constants;

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return mul(Constants.Transform, float4(position, 1.0));
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return float4(Constants.Data.Values[0].Direction, 1.0);
}
