#include <core/platform.hlsli>
struct SkyData { float4x4 ClipToWorld; };
VK_BINDING(0,0) ConstantBuffer<SkyData> SkyFrame : register(b0,space0);
struct Varying { float4 Position : SV_Position; float2 UV : TEXCOORD0; };
[shader("vertex")]
Varying VSMain(uint id : SV_VertexID) {
    Varying output;
    output.UV=float2((id<<1)&2,id&2);
    output.Position=float4(output.UV*float2(2,-2)+float2(-1,1),0,1);
    return output;
}
[shader("pixel")]
float4 PSMain(Varying input) : SV_Target0 {
    float2 clip=input.UV*float2(2,-2)+float2(-1,1);
    float4 nearPoint=mul(SkyFrame.ClipToWorld,float4(clip,0,1));
    float4 farPoint=mul(SkyFrame.ClipToWorld,float4(clip,1,1));
    // Homogeneous endpoint subtraction also supports parallel orthographic rays.
    float3 direction=normalize(farPoint.xyz*nearPoint.w-nearPoint.xyz*farPoint.w);
    float elevation=direction.y;
    float3 color=lerp(float3(.18,.30,.34),float3(.014,.028,.061),smoothstep(0,.65,elevation));
    color=lerp(float3(.04,.065,.075),color,smoothstep(-.25,0,elevation));
    color+=float3(.22,.10,.025)*pow(saturate(1-abs(elevation)*4),5);
    float longitude=dot(direction.xz,direction.xz)>1e-8?atan2(direction.x,direction.z):0;
    float2 sphere=float2(longitude/6.2831853,asin(clamp(elevation,-1,1))/3.1415927);
    uint2 cell=uint2(floor((sphere+.5)*float2(2400,1200)));
    uint hash=cell.x*1973u+cell.y*9277u+89173u;
    hash=(hash^(hash>>16))*0x7feb352du;
    hash=(hash^(hash>>15))*0x846ca68bu;
    hash^=hash>>16;
    float star=step(.9988,float(hash&0xffffffu)/16777216.0)*smoothstep(.05,.2,elevation);
    return float4(color+star*.25,1);
}
